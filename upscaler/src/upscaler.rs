//! Frame upscalers: an ONNX Runtime super-resolution model (Real-ESRGAN, etc.) and a
//! plain Lanczos fallback for comparison or for running without a model.

use std::fmt::Display;
use std::path::Path;
use std::time::Instant;

use anyhow::{Context, Result, anyhow, bail};
use half::f16;
use image::{RgbImage, imageops};
use ort::environment::Environment;
use ort::session::Session;
use ort::session::builder::GraphOptimizationLevel;
use ort::value::{TensorElementType, TensorRef, ValueType};

use crate::frame::Frame;

pub trait Upscaler {
    fn scale(&self) -> u32;
    fn upscale(&mut self, frame: &Frame) -> Result<Frame>;
}

fn ort_err(e: impl Display) -> anyhow::Error {
    anyhow!("onnxruntime: {e}")
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, clap::ValueEnum)]
pub enum Device {
    /// Best available GPU, falling back to CPU.
    Auto,
    Cpu,
    /// DirectX 12 GPU via DirectML (Windows; AMD, Intel or NVIDIA).
    Directml,
    /// NVIDIA GPU via CUDA. Requires `--features cuda` and the CUDA/cuDNN runtime.
    Cuda,
}

impl Device {
    /// GPU backends compiled into this build, in order of preference for `Auto`.
    fn gpu_candidates() -> Vec<Device> {
        let mut devices = Vec::new();
        if cfg!(feature = "cuda") {
            devices.push(Device::Cuda);
        }
        if cfg!(windows) {
            devices.push(Device::Directml);
        }
        devices
    }
}

/// Which inference devices really work on this PC (for `--capabilities`): each GPU backend
/// built into this binary must load a tiny test model, which fails without a usable GPU.
///
/// Also reports, under "errors", why a backend is unavailable: "not_built" when this binary
/// was compiled without it, otherwise the ONNX Runtime error (e.g. a missing CUDA/cuDNN DLL).
pub fn available_devices() -> serde_json::Value {
    let mut result = serde_json::Map::new();
    let mut errors = serde_json::Map::new();
    result.insert("cpu".into(), true.into());
    result.insert("directml".into(), false.into());
    result.insert("cuda".into(), false.into());
    for device in [Device::Directml, Device::Cuda] {
        if !Device::gpu_candidates().contains(&device) {
            errors.insert(format!("{device:?}").to_lowercase(), "not_built".into());
        }
    }

    match ort::init().with_name("ai-upscaler-probe").build() {
        Ok(environment) => {
            let probe_path = std::env::temp_dir().join(format!("ai-upscaler-probe-{}.onnx", std::process::id()));
            if std::fs::write(&probe_path, crate::testmodel::resize_x2_model()).is_ok() {
                for device in Device::gpu_candidates() {
                    let key = format!("{device:?}").to_lowercase();
                    match create_session(&environment, &probe_path, device, 0) {
                        Ok(_) => {
                            result.insert(key, true.into());
                        }
                        Err(e) => {
                            errors.insert(key, format!("{e:#}").into());
                        }
                    }
                }
                let _ = std::fs::remove_file(&probe_path);
            }
        }
        Err(e) => {
            errors.insert("runtime".into(), format!("{e:#}").into());
        }
    }
    result.insert("errors".into(), serde_json::Value::Object(errors));
    serde_json::Value::Object(result)
}

fn create_session(environment: &Environment, model: &Path, device: Device, threads: usize) -> Result<Session> {
    let mut builder = Session::builder(environment)
        .map_err(ort_err)?
        .with_optimization_level(GraphOptimizationLevel::Level3)
        .map_err(ort_err)?;
    if threads > 0 {
        builder = builder.with_intra_threads(threads).map_err(ort_err)?;
    }
    builder = match device {
        Device::Cpu => builder,
        #[cfg(windows)]
        Device::Directml => builder
            // DirectML does not support memory pattern optimisation or parallel execution.
            .with_memory_pattern(false)
            .map_err(ort_err)?
            .with_parallel_execution(false)
            .map_err(ort_err)?
            .with_execution_providers([ort::ep::DirectML::default().build().error_on_failure()])
            .map_err(ort_err)?,
        #[cfg(feature = "cuda")]
        Device::Cuda => builder
            .with_execution_providers([ort::ep::CUDA::default().build().error_on_failure()])
            .map_err(ort_err)?,
        #[cfg(not(windows))]
        Device::Directml => bail!("DirectML is only available on Windows"),
        #[cfg(not(feature = "cuda"))]
        Device::Cuda => bail!("this build has no CUDA support; rebuild with `--features cuda`"),
        Device::Auto => unreachable!("resolved by the caller"),
    };
    builder.commit_from_file(model).map_err(ort_err)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Precision {
    F32,
    F16,
}

impl Precision {
    fn from_value_type(ty: &ValueType, what: &str) -> Result<Self> {
        match ty {
            ValueType::Tensor { ty: TensorElementType::Float32, .. } => Ok(Self::F32),
            ValueType::Tensor { ty: TensorElementType::Float16, .. } => Ok(Self::F16),
            other => bail!("unsupported model {what} type {other:?}; expected a float32 or float16 tensor"),
        }
    }
}

/// Auto tiling (`--tile 0`) sizes windows so one inference call takes about this long.
/// Windows resets the GPU (TDR) when a single submission runs ~2 s, which a heavy model
/// such as RRDB x4plus on a whole DVD frame does; whole frames are otherwise fastest.
const AUTO_TILE_BUDGET_SECS: f64 = 0.8;
/// Auto tiling never puts more than this many input pixels (about 1080p) in one call.
const AUTO_TILE_MAX_PIXELS: usize = 1920 * 1088;
const AUTO_TILE_MIN_SIDE: usize = 64;

/// One tile of a frame: the model sees a `win_w`x`win_h` window starting at (x0, y0)
/// (edge-replicated where it leaves the frame) and its upscaled core lands in the output.
#[derive(Clone, Copy)]
struct TilePlan {
    x0: i64,
    y0: i64,
    core_x: usize,
    core_y: usize,
    core_w: usize,
    core_h: usize,
}

/// Borrowed model output, so DirectML results are converted straight into the frame.
enum OutputData<'a> {
    F32(&'a [f32]),
    F16(&'a [f16]),
}

pub struct OnnxUpscaler {
    _environment: Environment,
    session: Session,
    input_name: String,
    input_precision: Precision,
    output_precision: Precision,
    /// Some models are exported with a fixed (H, W) input; tiles are then sized to match.
    fixed_input: Option<(usize, usize)>,
    /// `None` when the model accepts any batch size.
    fixed_batch: Option<usize>,
    tile: u32,
    pad: u32,
    batch: usize,
    /// The model's output has no batch dimension ([3, H, W]), so it runs one window per call.
    unbatched_output: bool,
    scale: u32,
    input_buf: Vec<f32>,
    /// Largest window (in pixels) one call may use in auto-tile mode; set by `calibrate`.
    auto_max_pixels: usize,
    input_buf_f16: Vec<f16>,
}

impl OnnxUpscaler {
    pub fn new(model: &Path, device: Device, tile: u32, pad: u32, batch: usize, threads: usize) -> Result<Self> {
        let environment = ort::init().with_name("ai-upscaler").build().map_err(ort_err)?;
        let candidates = match device {
            Device::Auto => Device::gpu_candidates().into_iter().chain([Device::Cpu]).collect(),
            other => vec![other],
        };
        let mut session = None;
        for candidate in candidates {
            match create_session(&environment, model, candidate, threads) {
                Ok(s) => {
                    eprintln!("Inference device: {candidate:?}");
                    session = Some(s);
                    break;
                }
                Err(e) if device == Device::Auto && candidate != Device::Cpu => {
                    eprintln!("{candidate:?} unavailable ({e}); trying next device");
                }
                Err(e) => return Err(e).with_context(|| format!("failed to load model {}", model.display())),
            }
        }
        let session = session.context("no usable inference device")?;

        let input = session.inputs().first().context("model has no inputs")?;
        let output = session.outputs().first().context("model has no outputs")?;
        let input_name = input.name().to_string();
        let input_precision = Precision::from_value_type(input.dtype(), "input")?;
        let output_precision = Precision::from_value_type(output.dtype(), "output")?;

        let (fixed_batch, fixed_input) = match input.dtype() {
            ValueType::Tensor { shape, .. } if shape.len() == 4 => {
                if shape[1] > 0 && shape[1] != 3 {
                    bail!("model expects {} input channels; only RGB (3) models are supported", shape[1]);
                }
                (
                    (shape[0] > 0).then(|| shape[0] as usize),
                    (shape[2] > 0 && shape[3] > 0).then(|| (shape[2] as usize, shape[3] as usize)),
                )
            }
            ValueType::Tensor { shape, .. } => bail!("model input must be NCHW (4-D), got shape {shape:?}"),
            _ => unreachable!("checked by Precision::from_value_type"),
        };
        if let Some((h, w)) = fixed_input
            && (h <= 2 * pad as usize || w <= 2 * pad as usize)
        {
            bail!("model has fixed input {w}x{h}, too small for tile padding {pad}");
        }
        let describe = |d: Option<usize>| d.map_or("any".to_string(), |v| v.to_string());
        eprintln!(
            "Model input: batch {}, size {}",
            describe(fixed_batch),
            fixed_input.map_or("any".to_string(), |(h, w)| format!("{w}x{h}"))
        );

        let mut upscaler = Self {
            _environment: environment,
            session,
            input_name,
            input_precision,
            output_precision,
            fixed_input,
            fixed_batch,
            tile,
            pad,
            batch: fixed_batch.unwrap_or(batch.max(1)),
            unbatched_output: false,
            scale: 1,
            input_buf: Vec::new(),
            input_buf_f16: Vec::new(),
            auto_max_pixels: AUTO_TILE_MAX_PIXELS,
        };
        upscaler.scale = upscaler.detect_scale()?;
        if upscaler.unbatched_output {
            if upscaler.fixed_batch.is_some_and(|b| b != 1) {
                bail!("model has a fixed batch of {} but its output has no batch dimension", upscaler.fixed_batch.unwrap());
            }
            if upscaler.batch > 1 {
                eprintln!("Model output has no batch dimension; running one window per call");
            }
            upscaler.batch = 1;
        }
        if upscaler.fixed_input.is_none() && tile == 0 {
            upscaler.auto_max_pixels = upscaler.calibrate()?;
            let side = (upscaler.auto_max_pixels as f64).sqrt() as usize;
            eprintln!("Auto tiling: up to {} pixels per call (about {side}x{side})", upscaler.auto_max_pixels);
        }
        Ok(upscaler)
    }

    /// Runs a dummy input through the model to learn its upscale factor.
    fn detect_scale(&mut self) -> Result<u32> {
        let (h, w) = self.fixed_input.unwrap_or((16, 16));
        let n = self.fixed_batch.unwrap_or(1);
        self.input_buf.clear();
        self.input_buf.resize(n * 3 * h * w, 0.0);
        let (oh, ow) = self.infer(n, w, h, |shape, _| Ok((shape[2], shape[3])))?;
        if ow % w != 0 || oh % h != 0 || ow / w != oh / h {
            bail!("model output {ow}x{oh} is not an integer multiple of input {w}x{h}");
        }
        Ok((ow / w) as u32)
    }

    /// Estimates how many input pixels fit in [`AUTO_TILE_BUDGET_SECS`] per call by timing
    /// two window sizes and fitting `time = overhead + per_pixel * pixels`, so the fixed
    /// per-call cost (dispatch, readback) is not mistaken for per-pixel cost. The probe
    /// sizes are small enough to stay far below the GPU timeout even for heavy models.
    fn calibrate(&mut self) -> Result<usize> {
        const SIDES: [usize; 2] = [64, 160];
        let n = self.fixed_batch.unwrap_or(1);
        let mut times = [0.0f64; 2];
        for (i, side) in SIDES.into_iter().enumerate() {
            self.input_buf.clear();
            self.input_buf.resize(n * 3 * side * side, 0.5);
            // The first run compiles GPU shaders for this shape; keep the best of the next two.
            self.infer(n, side, side, |_, _| Ok(()))?;
            times[i] = f64::MAX;
            for _ in 0..2 {
                let start = Instant::now();
                self.infer(n, side, side, |_, _| Ok(()))?;
                times[i] = times[i].min(start.elapsed().as_secs_f64());
            }
        }
        let pixels = SIDES.map(|s| (n * s * s) as f64);
        let per_pixel = ((times[1] - times[0]) / (pixels[1] - pixels[0])).max(1e-12);
        let overhead = (times[0] - per_pixel * pixels[0]).max(0.0);
        let budget = ((AUTO_TILE_BUDGET_SECS - overhead).max(0.0) / per_pixel) as usize / n;
        Ok(budget.clamp(AUTO_TILE_MIN_SIDE * AUTO_TILE_MIN_SIDE, AUTO_TILE_MAX_PIXELS))
    }

    /// Splits the frame into tiles that all share one window size, so they can be batched.
    fn plan_tiles(&self, fw: usize, fh: usize) -> (Vec<TilePlan>, usize, usize) {
        let pad = self.pad as usize;
        let (core_w, core_h) = match (self.fixed_input, self.tile) {
            (Some((h, w)), _) => (w - 2 * pad, h - 2 * pad),
            // Auto: the whole frame when it fits the per-call time budget (fastest), otherwise
            // the largest square tiles that do.
            (None, 0) if (fw + 2 * pad) * (fh + 2 * pad) <= self.auto_max_pixels => (fw, fh),
            (None, 0) => {
                let side = ((self.auto_max_pixels as f64).sqrt() as usize).saturating_sub(2 * pad).max(AUTO_TILE_MIN_SIDE);
                (fw.div_ceil(fw.div_ceil(side)), fh.div_ceil(fh.div_ceil(side)))
            }
            (None, t) => {
                // Even split (e.g. 720 wide with tile 256 -> 3 x 240) instead of 256+256+208,
                // so no window is mostly edge-replicated padding.
                let t = t as usize;
                (fw.div_ceil(fw.div_ceil(t)), fh.div_ceil(fh.div_ceil(t)))
            }
        };
        let (win_h, win_w) = self.fixed_input.unwrap_or((core_h + 2 * pad, core_w + 2 * pad));

        let mut tiles = Vec::new();
        for ty in (0..fh).step_by(core_h) {
            for tx in (0..fw).step_by(core_w) {
                tiles.push(TilePlan {
                    x0: tx as i64 - pad as i64,
                    y0: ty as i64 - pad as i64,
                    core_x: tx,
                    core_y: ty,
                    core_w: core_w.min(fw - tx),
                    core_h: core_h.min(fh - ty),
                });
            }
        }
        (tiles, win_w, win_h)
    }

    /// Fills slot `slot` of the planar input batch with the tile's window, normalised to [0, 1].
    fn fill_input(&mut self, frame: &Frame, tile: &TilePlan, slot: usize, win_w: usize, win_h: usize) {
        const INV: f32 = 1.0 / 255.0;
        let (fw, fh) = (frame.width as usize, frame.height as usize);
        let plane = win_w * win_h;
        let (r_plane, rest) = self.input_buf[slot * 3 * plane..(slot + 1) * 3 * plane].split_at_mut(plane);
        let (g_plane, b_plane) = rest.split_at_mut(plane);

        // Precompute clamped source columns once per tile rather than per pixel.
        let cols: Vec<usize> = (0..win_w).map(|x| (tile.x0 + x as i64).clamp(0, fw as i64 - 1) as usize * 3).collect();
        for y in 0..win_h {
            let sy = (tile.y0 + y as i64).clamp(0, fh as i64 - 1) as usize;
            let row = &frame.data[sy * fw * 3..(sy + 1) * fw * 3];
            let dst = y * win_w;
            for (x, &sx) in cols.iter().enumerate() {
                r_plane[dst + x] = row[sx] as f32 * INV;
                g_plane[dst + x] = row[sx + 1] as f32 * INV;
                b_plane[dst + x] = row[sx + 2] as f32 * INV;
            }
        }
    }

    /// Runs the model on the first `n` windows in `input_buf` and hands the borrowed output
    /// (shape [n, 3, H, W]) to `consume`.
    fn infer<R>(&mut self, n: usize, win_w: usize, win_h: usize, consume: impl FnOnce(&[usize], OutputData) -> Result<R>) -> Result<R> {
        let shape = [n, 3, win_h, win_w];
        let len = n * 3 * win_h * win_w;
        let outputs = match self.input_precision {
            Precision::F32 => {
                let tensor = TensorRef::from_array_view((shape, &self.input_buf[..len])).map_err(ort_err)?;
                self.session.run(ort::inputs![self.input_name.as_str() => tensor]).map_err(ort_err)?
            }
            Precision::F16 => {
                self.input_buf_f16.clear();
                self.input_buf_f16.extend(self.input_buf[..len].iter().map(|&v| f16::from_f32(v)));
                let tensor = TensorRef::from_array_view((shape, &self.input_buf_f16[..])).map_err(ort_err)?;
                self.session.run(ort::inputs![self.input_name.as_str() => tensor]).map_err(ort_err)?
            }
        };
        let value = &outputs[0];
        let (out_shape, data) = match self.output_precision {
            Precision::F32 => {
                let (s, d) = value.try_extract_tensor::<f32>().map_err(ort_err)?;
                (s.iter().map(|&v| v as usize).collect::<Vec<_>>(), OutputData::F32(d))
            }
            Precision::F16 => {
                let (s, d) = value.try_extract_tensor::<f16>().map_err(ort_err)?;
                (s.iter().map(|&v| v as usize).collect::<Vec<_>>(), OutputData::F16(d))
            }
        };
        // Some exports squeeze the batch dimension away ([3, H, W]); with one window the data
        // layout is identical, so treat it as [1, 3, H, W]. Such models then run unbatched.
        let mut out_shape = out_shape;
        if out_shape.len() == 3 && out_shape[0] == 3 {
            if n != 1 {
                bail!("model output {out_shape:?} has no batch dimension; run it with --batch 1");
            }
            out_shape.insert(0, 1);
            self.unbatched_output = true;
        }
        if out_shape.len() != 4 || out_shape[0] != n || out_shape[1] != 3 {
            bail!("unexpected model output shape {out_shape:?}; expected [{n}, 3, H, W]");
        }
        consume(&out_shape, data)
    }
}

#[inline]
fn to_u8(v: f32) -> u8 {
    // Faster than clamp().round(); NaN maps to 0.
    (v * 255.0 + 0.5).clamp(0.0, 255.0) as u8
}

/// Copies the cores of a batch of upscaled tiles from planar model output into the
/// interleaved RGB output frame.
fn write_tiles<T: Copy>(data: &[T], conv: impl Fn(T) -> f32, tiles: &[TilePlan], tile_w: usize, tile_h: usize, s: usize, pad: usize, out: &mut Frame) {
    let out_w = out.width as usize;
    let plane = tile_w * tile_h;
    for (i, t) in tiles.iter().enumerate() {
        let base = i * 3 * plane;
        let (r, g, b) = (&data[base..base + plane], &data[base + plane..base + 2 * plane], &data[base + 2 * plane..base + 3 * plane]);
        let w = t.core_w * s;
        for y in 0..t.core_h * s {
            let src = (y + pad * s) * tile_w + pad * s;
            let dst = ((t.core_y * s + y) * out_w + t.core_x * s) * 3;
            let row = &mut out.data[dst..dst + w * 3];
            for (x, px) in row.chunks_exact_mut(3).enumerate() {
                px[0] = to_u8(conv(r[src + x]));
                px[1] = to_u8(conv(g[src + x]));
                px[2] = to_u8(conv(b[src + x]));
            }
        }
    }
}

impl Upscaler for OnnxUpscaler {
    fn scale(&self) -> u32 {
        self.scale
    }

    fn upscale(&mut self, frame: &Frame) -> Result<Frame> {
        let s = self.scale as usize;
        let pad = self.pad as usize;
        let (fw, fh) = (frame.width as usize, frame.height as usize);
        let mut out = Frame::new(frame.width * self.scale, frame.height * self.scale);
        let (tiles, win_w, win_h) = self.plan_tiles(fw, fh);

        for chunk in tiles.chunks(self.batch) {
            // A fixed-batch model needs exactly that many windows; repeat the last one.
            let n = self.fixed_batch.unwrap_or(chunk.len());
            self.input_buf.resize(n * 3 * win_w * win_h, 0.0);
            for slot in 0..n {
                let tile = chunk[slot.min(chunk.len() - 1)];
                self.fill_input(frame, &tile, slot, win_w, win_h);
            }

            let (tw, th) = (win_w * s, win_h * s);
            self.infer(n, win_w, win_h, |shape, data| {
                if shape[2] != th || shape[3] != tw {
                    bail!("model output {}x{} for a {win_w}x{win_h} input; expected x{s}", shape[3], shape[2]);
                }
                match data {
                    OutputData::F32(d) => write_tiles(d, |v| v, chunk, tw, th, s, pad, &mut out),
                    OutputData::F16(d) => write_tiles(d, f16::to_f32, chunk, tw, th, s, pad, &mut out),
                }
                Ok(())
            })?;
        }
        Ok(out)
    }
}

/// Non-AI baseline: Lanczos3 resampling.
pub struct LanczosUpscaler {
    pub scale: u32,
}

impl Upscaler for LanczosUpscaler {
    fn scale(&self) -> u32 {
        self.scale
    }

    fn upscale(&mut self, frame: &Frame) -> Result<Frame> {
        let img = RgbImage::from_raw(frame.width, frame.height, frame.data.clone()).context("bad frame buffer")?;
        let (w, h) = (frame.width * self.scale, frame.height * self.scale);
        let resized = imageops::resize(&img, w, h, imageops::FilterType::Lanczos3);
        Ok(Frame { width: w, height: h, data: resized.into_raw() })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Upscales a flat-colour frame with a generated x2 model on the CPU.
    fn run_x2(name: &str, model: Vec<u8>, batch: usize) -> (OnnxUpscaler, Frame) {
        let path = std::env::temp_dir().join(format!("ai-upscaler-test-{name}-{}.onnx", std::process::id()));
        std::fs::write(&path, model).unwrap();
        let mut upscaler = OnnxUpscaler::new(&path, Device::Cpu, 8, 2, batch, 1).unwrap();
        let _ = std::fs::remove_file(&path);
        let mut frame = Frame::new(20, 12);
        frame.data.chunks_exact_mut(3).for_each(|px| px.copy_from_slice(&[200, 100, 50]));
        let out = upscaler.upscale(&frame).unwrap();
        (upscaler, out)
    }

    fn assert_flat_x2(out: &Frame) {
        assert_eq!((out.width, out.height), (40, 24));
        assert!(out.data.chunks_exact(3).all(|px| px == [200, 100, 50]));
    }

    #[test]
    fn batched_output_model() {
        let (upscaler, out) = run_x2("batched", crate::testmodel::resize_x2_model(), 4);
        assert_eq!((upscaler.scale, upscaler.unbatched_output), (2, false));
        assert_flat_x2(&out);
    }

    #[test]
    fn output_without_batch_dimension() {
        let (upscaler, out) = run_x2("squeezed", crate::testmodel::resize_x2_squeezed_model(), 4);
        assert_eq!((upscaler.scale, upscaler.unbatched_output, upscaler.batch), (2, true, 1));
        assert_flat_x2(&out);
    }
}
