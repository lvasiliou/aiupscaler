mod frame;
mod geometry;
mod preview;
mod probe;
mod testmodel;
mod upscaler;
mod video;

use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use clap::Parser;
use serde_json::json;
use crossbeam_channel::bounded;
use indicatif::{ProgressBar, ProgressStyle};

use frame::Frame;
use geometry::Fit;
use upscaler::{Device, LanczosUpscaler, OnnxUpscaler, Upscaler};
use preview::PreviewWriter;
use video::{Decoder, Encoder, EncoderSettings};

/// AI video upscaler: runs every frame through an ONNX super-resolution model
/// (e.g. Real-ESRGAN) and re-muxes the original audio into the result.
#[derive(Parser, Debug)]
#[command(version, about)]
struct Args {
    /// Input video (anything ffmpeg can read) or a single PNG/JPEG image.
    #[arg(required_unless_present = "capabilities")]
    input: Option<PathBuf>,

    /// Output file. Defaults to `<input>_upscaled.<ext>`.
    #[arg(short, long)]
    output: Option<PathBuf>,

    /// ONNX super-resolution model (NCHW RGB in [0,1], e.g. Real-ESRGAN x4plus).
    /// Without a model, plain Lanczos resampling is used.
    #[arg(short, long)]
    model: Option<PathBuf>,

    /// Inference device.
    #[arg(short, long, value_enum, default_value_t = Device::Auto)]
    device: Device,

    /// Tile size in input pixels; smaller uses less GPU memory. 0 = auto (whole frame up to
    /// 1080p input, which is fastest; 512-pixel tiles above that).
    #[arg(long, default_value_t = 0)]
    tile: u32,

    /// Context pixels around each tile, discarded after inference to hide seams.
    #[arg(long, default_value_t = 10)]
    tile_pad: u32,

    /// Tiles per inference call (when the model allows any batch size). Higher can be faster
    /// on GPUs but uses more memory.
    #[arg(long, default_value_t = 1)]
    batch: usize,

    /// CPU threads for inference (0 = onnxruntime default).
    #[arg(long, default_value_t = 0)]
    threads: usize,

    /// Scale factor for the Lanczos fallback (ignored when a model is given).
    #[arg(long, default_value_t = 2)]
    scale: u32,

    /// Final frame size, e.g. 1920x1080 for 1080p. How a differently shaped picture is placed
    /// in it is set by --fit.
    #[arg(long, value_parser = geometry::parse_size, conflicts_with = "height")]
    size: Option<(u32, u32)>,

    /// How the picture fits a --size frame of a different shape: pad (black bars), crop,
    /// stretch, or keep (match the height, keep the picture's shape, no bars).
    #[arg(long, value_enum, default_value_t = Fit::Pad)]
    fit: Fit,

    /// Resize the AI output to this height, keeping its shape (same as --size with --fit keep).
    #[arg(long)]
    height: Option<u32>,

    /// Video encoder. `auto`, `auto-hevc` or `auto-av1` pick a GPU encoder (NVENC, AMF,
    /// QuickSync) and fall back to software; or name any ffmpeg encoder (libx264, h264_amf, ...).
    #[arg(long, default_value = "auto")]
    codec: String,

    /// Hardware video decoding passed to ffmpeg `-hwaccel` (auto, d3d11va, cuda, qsv, ...), or `none`.
    #[arg(long, default_value = "auto")]
    hwdec: String,

    /// Constant quality (lower = better). Maps to CRF, NVENC -cq, AMF QP or QSV global_quality.
    #[arg(long, default_value_t = 18)]
    crf: u32,

    /// Encoder preset for libx264/libx265.
    #[arg(long, default_value = "medium")]
    preset: String,

    /// Audio codec: `copy` keeps the original audio bit-for-bit; use e.g. `aac` if the
    /// output container can't hold the source audio format.
    #[arg(long, default_value = "copy")]
    audio_codec: String,

    /// Drop audio from the output.
    #[arg(long)]
    no_audio: bool,

    /// Only process the first N frames (handy for previews).
    #[arg(long)]
    max_frames: Option<u64>,

    /// Only process the first N seconds (converted to frames using the source frame rate).
    #[arg(long, conflicts_with = "max_frames")]
    max_seconds: Option<f64>,

    /// Extra arguments appended to the ffmpeg encoder command.
    #[arg(long, allow_hyphen_values = true, num_args = 1..)]
    encoder_args: Vec<String>,

    /// Path to the ffmpeg binary.
    #[arg(long, default_value = "ffmpeg")]
    ffmpeg: String,

    /// Path to the ffprobe binary.
    #[arg(long, default_value = "ffprobe")]
    ffprobe: String,

    /// Machine-readable mode for front ends: JSON-lines progress events on stdout, and a
    /// `cancel` line on stdin (or stdin closing) stops the job and deletes the partial output.
    #[arg(long)]
    json: bool,

    /// Print the input's stream info as one JSON line and exit.
    #[arg(long)]
    probe: bool,

    /// Upscale only the frame at this time (seconds) and write source/output images to
    /// --preview-dir, reported as a `preview` JSON event.
    #[arg(long, requires = "preview_dir")]
    frame_at: Option<f64>,

    /// With --frame-at: only extract the source frame, no upscaling (fast scrubbing).
    #[arg(long, requires = "frame_at")]
    source_only: bool,

    /// Folder for preview images. During a video run, a source/output pair is written there
    /// every --preview-interval seconds and reported as a `preview` JSON event.
    #[arg(long)]
    preview_dir: Option<PathBuf>,

    /// Seconds between live previews during a video run.
    #[arg(long, default_value_t = 2.0)]
    preview_interval: f64,

    /// Write N evenly spaced thumbnails of the input to --preview-dir (for a film strip),
    /// each reported as a `thumbnail` JSON event as soon as it is ready.
    #[arg(long, requires = "preview_dir")]
    thumbnails: Option<u32>,

    /// Height of --thumbnails images in pixels.
    #[arg(long, default_value_t = 96)]
    thumb_height: u32,

    /// Report what this PC supports (AI devices, working video encoders) as one JSON line.
    #[arg(long)]
    capabilities: bool,
}

impl Args {
    fn input(&self) -> &Path {
        self.input.as_deref().expect("clap requires an input unless --capabilities")
    }
}

static CANCELLED: AtomicBool = AtomicBool::new(false);

fn emit(args: &Args, event: serde_json::Value) {
    emit_if(args.json, event);
}

fn emit_if(enabled: bool, event: serde_json::Value) {
    if enabled {
        let mut out = std::io::stdout().lock();
        let _ = writeln!(out, "{event}");
        let _ = out.flush();
    }
}

fn watch_for_cancel() {
    std::thread::spawn(|| {
        for line in std::io::stdin().lines() {
            match line {
                Ok(l) if l.trim() == "cancel" => {
                    CANCELLED.store(true, Ordering::Relaxed);
                    return;
                }
                Ok(_) => {}
                Err(_) => break,
            }
        }
        // stdin closed: the front end went away, so stop rather than run orphaned.
        CANCELLED.store(true, Ordering::Relaxed);
    });
}

fn is_image(path: &Path) -> bool {
    matches!(
        path.extension().and_then(|e| e.to_str()).map(str::to_ascii_lowercase).as_deref(),
        Some("png" | "jpg" | "jpeg")
    )
}

fn default_output(input: &Path) -> PathBuf {
    let stem = input.file_stem().and_then(|s| s.to_str()).unwrap_or("output");
    let ext = if is_image(input) { "png" } else { "mp4" };
    input.with_file_name(format!("{stem}_upscaled.{ext}"))
}

fn build_upscaler(args: &Args) -> Result<Box<dyn Upscaler>> {
    match &args.model {
        Some(model) => {
            eprintln!("Loading model {} on {:?}...", model.display(), args.device);
            let up = OnnxUpscaler::new(model, args.device, args.tile, args.tile_pad, args.batch, args.threads)?;
            eprintln!("Model upscale factor: x{}", up.scale());
            Ok(Box::new(up))
        }
        None => {
            eprintln!("No --model given; using Lanczos x{} (not AI).", args.scale);
            Ok(Box::new(LanczosUpscaler { scale: args.scale.max(1) }))
        }
    }
}

fn upscale_image(args: &Args, output: &Path) -> Result<()> {
    let img = image::open(args.input()).with_context(|| format!("failed to open {}", args.input().display()))?.to_rgb8();
    let frame = Frame { width: img.width(), height: img.height(), data: img.into_raw() };
    let mut upscaler = build_upscaler(args)?;
    let start = Instant::now();
    let out = upscaler.upscale(&frame)?;
    image::RgbImage::from_raw(out.width, out.height, out.data)
        .context("bad output buffer")?
        .save(output)
        .with_context(|| format!("failed to write {}", output.display()))?;
    eprintln!(
        "{}x{} -> {}x{} in {:.2?}: {}",
        frame.width, frame.height, out.width, out.height, start.elapsed(), output.display()
    );
    Ok(())
}

fn preview_event(time: f64, frame: Option<u64>, source_path: &Path, output_path: Option<&Path>, source: &Frame, output: Option<&Frame>) -> serde_json::Value {
    json!({
        "event": "preview",
        "time": time,
        "frame": frame,
        "source": source_path.display().to_string(),
        "source_width": source.width,
        "source_height": source.height,
        "output": output_path.map(|p| p.display().to_string()),
        "output_width": output.map(|f| f.width),
        "output_height": output.map(|f| f.height),
    })
}

/// `--probe`: stream info for front ends (always printed, with or without --json).
fn probe_only(args: &Args) -> Result<()> {
    let info = probe::probe(&args.ffprobe, args.input())?;
    emit_if(
        true,
        json!({
            "event": "probe",
            "width": info.width,
            "height": info.height,
            "fps": info.fps_value,
            "frames": info.frame_count,
            "duration": info.duration_secs,
            "sar": info.sar,
            "audio": info.has_audio,
        }),
    );
    Ok(())
}

/// `--frame-at`: decode one frame at the given time and, unless --source-only, upscale it.
fn frame_preview(args: &Args, time: f64) -> Result<()> {
    let dir = args.preview_dir.as_deref().context("--frame-at needs --preview-dir")?;
    let mut writer = PreviewWriter::new(dir)?;
    let info = probe::probe(&args.ffprobe, args.input())?;
    let time = if info.duration_secs > 0.0 { time.clamp(0.0, (info.duration_secs - 1.0 / info.fps_value).max(0.0)) } else { time.max(0.0) };

    let mut decoder = Decoder::spawn(&args.ffmpeg, args.input(), info.width, info.height, &info.fps, &args.hwdec, Some(time))?;
    let source = decoder.next_frame()?.with_context(|| format!("no frame at {time:.2}s"))?;
    decoder.kill();

    let output = if args.source_only {
        None
    } else {
        let mut upscaler = build_upscaler(args)?;
        let start = Instant::now();
        let out = upscaler.upscale(&source)?;
        eprintln!("Frame at {time:.2}s: {}x{} -> {}x{} in {:.2?}", source.width, source.height, out.width, out.height, start.elapsed());
        Some(out)
    };

    // Millisecond timestamps keep file names unique across scrubs.
    let id = (time * 1000.0).round() as u64;
    let (src_path, out_path) = writer.write(id, &source, output.as_ref())?;
    emit_if(true, preview_event(time, None, &src_path, out_path.as_deref(), &source, output.as_ref()));
    Ok(())
}

fn upscale_video(args: &Args, output: &Path) -> Result<()> {
    let info = probe::probe(&args.ffprobe, args.input())?;
    let mut upscaler = build_upscaler(args)?;
    let s = upscaler.scale();
    let (out_w, out_h) = (info.width * s, info.height * s);
    let include_audio = info.has_audio && !args.no_audio;
    let frame_limit = args.max_frames.or(args.max_seconds.map(|s| (s * info.fps_value).ceil().max(1.0) as u64));
    let geometry = geometry::output_geometry(out_w, out_h, info.sar, args.size, args.height, args.fit);

    let settings = EncoderSettings {
        codec: video::resolve_codec(&args.ffmpeg, &args.codec),
        crf: args.crf,
        preset: args.preset.clone(),
        audio_codec: args.audio_codec.clone(),
        video_filter: geometry.filter.clone(),
        duration_limit: frame_limit.map(|n| n as f64 / info.fps_value),
        extra_args: args.encoder_args.clone(),
    };

    eprintln!(
        "{}x{} @ {:.3} fps -> AI {}x{} -> output {}x{} | encoder: {} | hwdec: {} | audio: {}",
        info.width,
        info.height,
        info.fps_value,
        out_w,
        out_h,
        geometry.width,
        geometry.height,
        settings.codec,
        args.hwdec,
        if include_audio { args.audio_codec.as_str() } else { "none" },
    );

    let total = match (frame_limit, info.frame_count) {
        (Some(m), 0) => m,
        (Some(m), n) => m.min(n),
        (None, n) => n,
    };
    emit(
        args,
        json!({
            "event": "start",
            "width": info.width,
            "height": info.height,
            "out_width": out_w,
            "out_height": out_h,
            "final_width": geometry.width,
            "final_height": geometry.height,
            "fps": info.fps_value,
            "frames": total,
            "source_frames": info.frame_count,
            "encoder": settings.codec,
            "audio": include_audio,
        }),
    );

    let progress = if args.json {
        ProgressBar::hidden()
    } else if total > 0 {
        ProgressBar::new(total)
    } else {
        ProgressBar::new_spinner()
    };
    progress.set_style(
        ProgressStyle::with_template("{spinner} [{elapsed_precise}] {wide_bar} {pos}/{len} frames  {per_sec}  eta {eta}")
            .unwrap(),
    );

    let mut decoder = Decoder::spawn(&args.ffmpeg, args.input(), info.width, info.height, &info.fps, &args.hwdec, None)?;

    // Live previews are JPEG-encoded on their own thread; if it is still busy when the next
    // one is due, that preview is skipped rather than slowing the upscale.
    let (preview_tx, preview_thread) = match &args.preview_dir {
        Some(dir) => {
            let mut writer = PreviewWriter::new(dir)?;
            let (tx, rx) = bounded::<(u64, Frame, Frame)>(1);
            let (json_enabled, fps) = (args.json, info.fps_value);
            let thread = std::thread::spawn(move || {
                for (index, source, output) in rx {
                    match writer.write(index, &source, Some(&output)) {
                        Ok((src, out)) => emit_if(json_enabled, preview_event(index as f64 / fps, Some(index), &src, out.as_deref(), &source, Some(&output))),
                        Err(e) => eprintln!("preview: {e:#}"),
                    }
                }
            });
            (Some(tx), Some(thread))
        }
        None => (None, None),
    };
    let preview_interval = Duration::from_secs_f64(args.preview_interval.max(0.1));
    let mut last_preview: Option<Instant> = None;

    let encoder = Encoder::spawn(&args.ffmpeg, args.input(), output, out_w, out_h, &info.fps, include_audio, &settings)?;

    // decode thread -> (inference on this thread) -> encode thread, with small bounded
    // queues so ffmpeg I/O overlaps with the model without buffering the whole video.
    let (decoded_tx, decoded_rx) = bounded::<Frame>(4);
    let (upscaled_tx, upscaled_rx) = bounded::<Frame>(4);
    let max_frames = frame_limit.unwrap_or(u64::MAX);

    let decode_thread = std::thread::spawn(move || -> Result<()> {
        let mut n = 0;
        let mut stopped_early = false;
        while n < max_frames {
            let Some(frame) = decoder.next_frame()? else { break };
            if decoded_tx.send(frame).is_err() {
                stopped_early = true; // downstream stopped (error or cancel)
                break;
            }
            n += 1;
        }
        drop(decoded_tx);
        if n < max_frames && !stopped_early {
            decoder.finish()
        } else {
            decoder.kill();
            Ok(())
        }
    });

    let encode_thread = std::thread::spawn(move || -> Result<()> {
        let mut encoder = encoder;
        for frame in upscaled_rx {
            if let Err(e) = encoder.write_frame(&frame) {
                let log = encoder.abort();
                bail!("{e}\n{}", log.trim());
            }
        }
        encoder.finish()
    });

    let start = Instant::now();
    let mut processed = 0u64;
    let mut infer_result = Ok(());
    let mut last_report = Instant::now() - Duration::from_secs(1);
    // Where the time goes: waiting for decoded frames, running the model, or waiting for the
    // encoder to take frames. Whichever dominates is the bottleneck.
    let (mut decode_wait, mut ai_time, mut encode_wait) = (Duration::ZERO, Duration::ZERO, Duration::ZERO);
    let mut last_stage_log = Instant::now();
    loop {
        let t = Instant::now();
        let Ok(frame) = decoded_rx.recv() else { break };
        decode_wait += t.elapsed();
        if CANCELLED.load(Ordering::Relaxed) {
            break;
        }
        let t = Instant::now();
        let result = upscaler.upscale(&frame);
        ai_time += t.elapsed();
        match result {
            Ok(up) => {
                if let Some(tx) = &preview_tx
                    && last_preview.is_none_or(|t| t.elapsed() >= preview_interval)
                    && tx.try_send((processed, frame, up.clone())).is_ok()
                {
                    last_preview = Some(Instant::now());
                }
                let t = Instant::now();
                if upscaled_tx.send(up).is_err() {
                    break; // encoder died; its error is reported below
                }
                encode_wait += t.elapsed();
            }
            Err(e) => {
                infer_result = Err(e);
                break;
            }
        }
        processed += 1;
        progress.inc(1);
        if processed == 1 || last_stage_log.elapsed() >= Duration::from_secs(10) {
            last_stage_log = Instant::now();
            eprintln!("{}", stage_summary(processed, decode_wait, ai_time, encode_wait));
        }
        if args.json && (last_report.elapsed() >= Duration::from_millis(250) || processed == total) {
            last_report = Instant::now();
            let fps = processed as f64 / start.elapsed().as_secs_f64();
            let eta = (total > processed).then(|| (total - processed) as f64 / fps);
            emit(args, json!({ "event": "progress", "frame": processed, "total": total, "fps": fps, "eta_secs": eta }));
        }
    }
    drop(upscaled_tx);
    drop(preview_tx);
    progress.finish_and_clear();
    if let Some(thread) = preview_thread {
        let _ = thread.join();
    }

    let encode_result = encode_thread.join().expect("encoder thread panicked");
    let decode_result = decode_thread.join().expect("decoder thread panicked");
    infer_result.context("upscaling failed")?;
    encode_result?;
    decode_result?;

    if CANCELLED.load(Ordering::Relaxed) {
        let _ = std::fs::remove_file(output);
        emit(args, json!({ "event": "cancelled", "frames": processed }));
        eprintln!("Cancelled after {processed} frames.");
        std::process::exit(130);
    }
    if processed == 0 {
        bail!("no frames were decoded");
    }
    let secs = start.elapsed().as_secs_f64();
    eprintln!("{}", stage_summary(processed, decode_wait, ai_time, encode_wait));
    eprintln!(
        "Done: {processed} frames in {:.1}s ({:.2} fps) -> {}",
        secs,
        processed as f64 / secs,
        output.display()
    );
    emit(args, json!({ "event": "done", "frames": processed, "secs": secs, "output": output.display().to_string() }));
    Ok(())
}

/// Average per-frame time in each pipeline stage, e.g. "Per frame after 120: AI 310 ms,
/// waiting for decoder 2 ms, waiting for encoder 45 ms".
fn stage_summary(frames: u64, decode_wait: Duration, ai: Duration, encode_wait: Duration) -> String {
    let ms = |d: Duration| d.as_secs_f64() * 1000.0 / frames.max(1) as f64;
    format!(
        "Per frame after {frames}: AI {:.0} ms, waiting for decoder {:.0} ms, waiting for encoder {:.0} ms",
        ms(ai),
        ms(decode_wait),
        ms(encode_wait)
    )
}

/// `--capabilities`: which AI devices and video encoders actually work on this PC, so front
/// ends can disable options that would fail.
fn capabilities(args: &Args) -> Result<()> {
    let encoders = video::probe_encoders(&args.ffmpeg);
    emit_if(
        true,
        json!({
            "event": "capabilities",
            "devices": upscaler::available_devices(),
            "encoders": encoders,
        }),
    );
    Ok(())
}

/// `--thumbnails N`: evenly spaced frames for a film strip. Decoding runs a few at a time
/// (each is its own ffmpeg seek) and every thumbnail is reported as soon as it is saved.
fn thumbnails(args: &Args, count: u32) -> Result<()> {
    const PARALLEL: usize = 4;
    let dir = args.preview_dir.as_deref().context("--thumbnails needs --preview-dir")?;
    std::fs::create_dir_all(dir)?;
    let info = probe::probe(&args.ffprobe, args.input())?;
    if info.duration_secs <= 0.0 {
        bail!("cannot make thumbnails: unknown duration");
    }
    let count = count.clamp(1, 200) as usize;
    // Thumbnails show the picture's true shape, so anamorphic DVDs are widened here too.
    let thumb_h = args.thumb_height.max(16);
    let thumb_w = ((info.width as f64 * info.sar / info.height as f64) * thumb_h as f64).round().max(16.0) as u32;

    let indices: Vec<usize> = (0..count).collect();
    std::thread::scope(|scope| {
        for chunk in indices.chunks(count.div_ceil(PARALLEL)) {
            let info = &info;
            scope.spawn(move || {
                for &i in chunk {
                    let time = info.duration_secs * (i as f64 + 0.5) / count as f64;
                    let result = (|| -> Result<PathBuf> {
                        let mut decoder = Decoder::spawn(&args.ffmpeg, args.input(), info.width, info.height, &info.fps, "none", Some(time))?;
                        let frame = decoder.next_frame()?.context("no frame")?;
                        decoder.kill();
                        let image = image::RgbImage::from_raw(frame.width, frame.height, frame.data).context("bad frame")?;
                        let thumb = image::imageops::thumbnail(&image, thumb_w, thumb_h);
                        let path = dir.join(format!("thumb_{i}.jpg"));
                        preview::save_jpeg(&path, &Frame { width: thumb.width(), height: thumb.height(), data: thumb.into_raw() })?;
                        Ok(path)
                    })();
                    match result {
                        Ok(path) => emit_if(true, json!({ "event": "thumbnail", "index": i, "count": count, "time": time, "path": path.display().to_string() })),
                        Err(e) => eprintln!("thumbnail {i} at {time:.1}s: {e:#}"),
                    }
                }
            });
        }
    });
    Ok(())
}

fn main() -> Result<()> {
    let args = Args::parse();
    if args.capabilities {
        return capabilities(&args);
    }
    if !args.input().exists() {
        bail!("input not found: {}", args.input().display());
    }
    if args.probe {
        return probe_only(&args);
    }
    if let Some(count) = args.thumbnails {
        return thumbnails(&args, count);
    }
    if let Some(time) = args.frame_at {
        return frame_preview(&args, time);
    }
    let output = args.output.clone().unwrap_or_else(|| default_output(args.input()));
    if output.as_path() == args.input() {
        bail!("output path must differ from input");
    }
    if args.json {
        watch_for_cancel();
    }

    if is_image(args.input()) { upscale_image(&args, &output) } else { upscale_video(&args, &output) }
}
