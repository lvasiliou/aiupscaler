# ai-upscaler

A Rust AI video upscaler. Every frame is run through an ONNX super-resolution
model (Real-ESRGAN or similar), and the original audio is muxed back in.

```
ffmpeg decode ──RGB24──▶ ONNX model (tiled) ──RGB24──▶ ffmpeg encode ──▶ output.mp4
   (thread)               (main thread)                  (thread)   ▲
                                                                      └─ audio + metadata from the source file
```

## Requirements

- Rust (stable)
- `ffmpeg` and `ffprobe` on `PATH` (or pass `--ffmpeg` / `--ffprobe`), e.g. `winget install Gyan.FFmpeg`
- An ONNX super-resolution model with NCHW RGB input in `[0, 1]`, float32 or float16.
  The scale factor, batch support and fixed/dynamic input size are detected
  automatically. See [Choosing a model](#choosing-a-model).

ONNX Runtime itself is downloaded by the `ort` crate at build time.

## Build

```bash
cargo build --release                  # CPU + DirectML GPU (on Windows)
cargo build --release --features cuda  # also NVIDIA CUDA (needs CUDA + cuDNN)
```

If the Windows linker fails with `LNK1104` the path is too long; set a short
`CARGO_TARGET_DIR` (e.g. `C:\t\aiup`).

## Usage

```bash
# 4x upscale, everything on the GPU by default (decode, AI, encode), keeping the audio
ai-upscaler input.mp4 -m RealESRGAN_x4plus.onnx -o output.mp4

# x4 model, but deliver 1080p; HEVC output
ai-upscaler input.mkv -m RealESRGAN_x4plus.onnx --height 1080 --codec auto-hevc --crf 20

# Best-quality software encode instead of GPU
ai-upscaler input.mp4 -m model.onnx --codec libx264 --preset slow

# Quick 100-frame preview
ai-upscaler input.mp4 -m model.onnx --max-frames 100 -o preview.mp4

# Single image (handy for testing a model)
ai-upscaler frame.png -m model.onnx

# No model: Lanczos baseline for comparison
ai-upscaler input.mp4 --scale 2
```

Key options (see `--help` for all):

| Option | Default | Notes |
| --- | --- | --- |
| `-m, --model` | — | ONNX model; without it, Lanczos is used |
| `-d, --device` | `auto` | `auto` (GPU, else CPU), `cpu`, `directml`, `cuda` |
| `--hwdec` | `auto` | ffmpeg `-hwaccel` (`d3d11va`, `cuda`, `qsv`, ...) or `none` |
| `--tile` / `--tile-pad` | `0` / `10` | `0` = auto: whole frame up to 1080p input (fastest), 512-px tiles above. Smaller tiles use less VRAM; padding hides seams |
| `--batch` | `1` | Tiles per inference call for models with a dynamic batch dimension |
| `--max-frames` / `--max-seconds` | | Only process the start of the video (previews); audio is trimmed to match |
| `--preview-dir` / `--preview-interval` | — / `2` | During a run, save source/output JPEG pairs there and emit `preview` JSON events |
| `--frame-at <secs>` [`--source-only`] | | Upscale (or just extract) one frame into `--preview-dir`; for scrubbing and frame previews |
| `--probe` | | Print the input's size, frame rate, frame count and duration as JSON |
| `--size WxH` / `--fit` | — / `pad` | Final frame size, e.g. `1920x1080`. `--fit` places a differently shaped picture: `pad` (black bars), `crop`, `stretch`, or `keep` (match the height, no bars) |
| `--height` | — | Resize to this height keeping the picture's shape (same as `--size` with `--fit keep`) |
| `--thumbnails N` | | Write N evenly spaced thumbnails to `--preview-dir` (film strip), as `thumbnail` events |
| `--capabilities` | | Report which AI devices and video encoders actually work on this PC, as JSON |
| `--codec` | `auto` | `auto`/`auto-hevc`/`auto-av1` = first working GPU encoder (NVENC, AMF, QSV), else software; or any ffmpeg encoder |
| `--crf`, `--preset` | `18`, `medium` | `--crf` maps to each encoder's constant-quality mode; `--preset` is for libx264/libx265 |
| `--audio-codec` | `copy` | Use `aac` if the output container can't hold the source audio |
| `--no-audio` | | Drop audio |
| `--encoder-args ...` | | Extra args appended to the ffmpeg encoder command |

## Notes

- Anamorphic sources (non-square pixels, common on DVD: 720×480 shown at 4:3 or 16:9)
  are resampled to square pixels, so the output has the correct picture shape.
- A 4:3 source with `--size 1920x1080` produces a true 1920×1080 file with black side
  bars by default; use `--fit keep` for a 1440×1080 file without bars.
- Variable-frame-rate input is converted to constant frame rate (the source's
  average rate) so audio stays in sync.
- Frames go through 8-bit RGB and output is `yuv420p`; HDR sources are not
  tone-mapped and will look washed out.
- Subtitles and chapters are not carried over yet.

## GPU acceleration

All three stages can run on the GPU:

| Stage | How | Default |
| --- | --- | --- |
| Decode | ffmpeg `-hwaccel` (D3D11VA/DXVA2, CUDA, QSV, ...) | `--hwdec auto`, falls back to software |
| AI inference | ONNX Runtime DirectML (Windows, any DX12 GPU) or CUDA | `--device auto`, falls back to CPU |
| Encode | NVENC (NVIDIA), AMF (AMD), QuickSync (Intel) | `--codec auto`, falls back to libx264 |

Inference is by far the slowest stage, so the model choice matters most (see below).
If you hit GPU memory errors, set `--tile` to e.g. 256 or 128.

To test the GPU path without a real model:

```bash
cargo run --release --example make_test_model -- test_x2.onnx
ai-upscaler frame.png -m test_x2.onnx
```

## Choosing a model

The model is by far the biggest factor in speed. Measured on an AMD Radeon 840M
iGPU (DirectML, `h264_amf` encode), synthetic 720×480 DVD-size clip, ×4 output:

| Model | Architecture | Input | fps (720×480) | Notes |
| --- | --- | --- | --- | --- |
| [RealESR_Gx4_fp16](https://huggingface.co/FuryTMP/RealESR_Gx4_fp16) | SRVGGNet (general-x4v3), **fp16** | dynamic | **2.98** | **Recommended.** Output matches the official fp32 weights at 59.6 dB PSNR. Community export |
| [realesr-general-x4v3](https://huggingface.co/jonathanst29/tinier-upscale-models) | SRVGGNet, fp32 | dynamic | 1.63 | Same model in fp32 |
| [Real-ESRGAN-General-x4v3](https://huggingface.co/qualcomm/Real-ESRGAN-General-x4v3) | SRVGGNet, fp32 | fixed 128×128 | 1.10 | Official Qualcomm export; fixed tiles cost ~30% |
| [Real-ESRGAN-x4plus](https://huggingface.co/qualcomm/Real-ESRGAN-x4plus) | RRDB, fp32 | fixed 128×128 | 0.09 | Heaviest; ~20× slower for modest gains on video |

Rules of thumb:

- **Prefer SRVGGNet (`general-x4v3`, `animevideov3` for anime) over RRDB (`x4plus`)** for video.
- **Prefer fp16 exports**: on GPUs with fast half precision (RDNA, recent NVIDIA/Intel) they
  run about twice as fast as fp32.
- **Prefer dynamic input sizes**: whole frames avoid tile overhead. With `--tile 0` (the
  default) the engine times a probe at startup and runs whole frames when one call
  fits a ~0.8 s budget, otherwise the largest tiles that do. This keeps heavy models under
  the Windows GPU timeout (TDR, ~2 s), which otherwise resets the driver
  (`DXGI_ERROR_DEVICE_HUNG 0x887A0007`).
- Resizing the ×4 output down (`--height 1080`) costs some speed (2.98 → 2.11 fps) for
  scaling and encoding the larger intermediate frames.

A real 576×432 DVD rip upscaled to 1080p with the fp16 model ran at 2.7 fps, about
6 h 40 min for a 45-minute episode. Use `--max-seconds 10` (or Preview in the app) to
check quality and the time estimate before a full run.

### Before/after engine changes

Same GPU and 720×480 clip. The engine now batches tiles into one call when the model
allows it, fills inputs straight from the frame, converts outputs from the borrowed
DirectML buffer without copying, and splits tiles evenly.

| Setup | Before | After |
| --- | --- | --- |
| Original setup: Qualcomm x4plus, 128×128 tiles | 0.09 fps | — |
| Qualcomm general-x4v3, 128×128 tiles | 0.65 fps | 1.10 fps |
| general-x4v3 fp32, whole frame | 1.40 fps | 1.63 fps |
| general-x4v3 fp16, whole frame (new default recommendation) | — | 2.98 fps |

Batching (`--batch 4`) measured no gain on this iGPU (1.39 vs 1.40 fps), because inference
is GPU-bound. It is kept for larger GPUs and defaults to 1.
