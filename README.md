# AI Upscaler

AI video upscaling for Windows: a Rust engine that runs super-resolution models
(Real-ESRGAN and friends) on the GPU, and a native WinUI 3 studio app to preview,
compare and upscale whole videos with their original audio.

```
Source video ──ffmpeg decode──▶ ONNX model on GPU ──ffmpeg encode──▶ upscaled video
                                (DirectML / CUDA)                      + original audio
```

## Highlights

- **GPU end to end**: hardware decode, AI inference on any DirectX 12 GPU (AMD, Intel, NVIDIA)
  through DirectML or on NVIDIA through CUDA, and hardware encode (AMF, NVENC, Quick Sync).
- **Studio app**: source on the left, AI result on the right, synced zoom and pan, a film-strip
  timeline, and automatic AI previews of the frame you stop on.
- **Real output sizes**: 720p, 1080p, 1440p and 4K produce the standard 16:9 frames
  (1280×720 … 3840×2160), with black bars, crop or stretch for other picture shapes.
  Anamorphic DVDs come out at the correct shape.
- **Original audio kept**, bit for bit, and in sync, or **improved**: clearer dialogue, even
  volume, noise and hiss removal, and stereo-to-5.1 upmix.
- **Previews and estimates**: upscale the first 10/30/60 seconds to check quality and see how
  long the whole video will take before committing.
- **Batch processing**: queue many videos (or a whole folder) and upscale them one after another.
- **Configurations**: save the upscale settings to a file and load them later; closing with
  unsaved changes asks to save them.
- **Model library**: import `.onnx` models once, then pick them by name.
- **Hardware-aware**: options this PC can't run are greyed out with the reason.
- English and Greek UI, light and dark themes.

## Requirements

- Windows 10 version 2004 (build 19041) or later, or Windows 11; 64-bit.
- A DirectX 12 GPU for fast AI (the CPU works, slowly).
- [FFmpeg](https://ffmpeg.org) on `PATH`: `winget install Gyan.FFmpeg`.
- An ONNX super-resolution model (see [Models](#models)).

To build from source you also need:

- [Rust](https://rustup.rs) (stable).
- Visual Studio 2022 or 2026 with the **Desktop development with C++** workload and a
  Windows 10/11 SDK.

## Build

```powershell
.\build.ps1
```

This builds the engine, downloads the Windows App SDK NuGet packages (no `nuget.exe`
needed) and builds the app. The result is a self-contained app, with no installer or runtime
to install:

```
%LOCALAPPDATA%\AiUpscalerBuild\gui\bin\AiUpscaler.exe
```

Builds go to a short path outside `%TEMP%` on purpose: the C++ toolchain breaks on paths
over 260 characters. Pass `-BuildRoot <folder>` to use another short folder.

### NVIDIA CUDA

The default build runs AI on any GPU through DirectML, NVIDIA cards included. For the CUDA
backend (often faster on NVIDIA):

```powershell
.\build.ps1 -Cuda
```

This produces `%LOCALAPPDATA%\AiUpscalerBuild\gui-cuda\bin\AiUpscaler.exe`, which includes
both CUDA and DirectML. The PC that runs it needs:

- a current NVIDIA driver,
- the [CUDA Toolkit 13.x](https://developer.nvidia.com/cuda-downloads) (or just its runtime),
- [cuDNN 9 for CUDA 13](https://developer.nvidia.com/cudnn-downloads),

with the `bin` folders of both on `PATH`. If something is missing, the app greys out
**GPU – CUDA** and its tooltip and the log say what failed; DirectML keeps working.

## Using the app

1. Drop a video on the **Source** pane (or press Ctrl+O, or run `AiUpscaler.exe <video>`).
2. Pick a frame on the film strip or timeline. Its AI preview appears on the right after
   3 seconds. Toggle **Fit / 1:1** to compare pixels; zooming and panning are synced.
3. In **Upscale settings**, choose the model, output size, picture shape, encoder and quality.
4. Optionally run **Preview: first 10 s** to check the result and the time estimate.
5. Press **Upscale**. When it's done, **Open** plays the file.

App **Settings** (the gear button) has the theme, language, default output folder and the
model library.

## Using the engine directly

The engine is a standalone command-line tool, `ai-upscaler.exe`:

```powershell
# 4x AI upscale to a 1080p frame, everything on the GPU, original audio kept
ai-upscaler input.mkv -m RealESR_Gx4_fp16.onnx --size 1920x1080 -o output.mp4

# Check the first 10 seconds first
ai-upscaler input.mkv -m model.onnx --size 1920x1080 --max-seconds 10 -o preview.mp4

# What this PC supports
ai-upscaler --capabilities
```

See [upscaler/README.md](upscaler/README.md) for every option and the JSON protocol the app uses.

## Models

The engine runs any image super-resolution model exported to ONNX with one RGB input and
output (NCHW, values 0–1), in fp32 or fp16. The scale factor is detected automatically.

Model choice matters most for speed. Measured on an AMD Radeon 840M (integrated GPU),
720×480 DVD source, ×4:

| Model | fps | 45-minute episode |
| --- | --- | --- |
| [RealESR_Gx4_fp16](https://huggingface.co/FuryTMP/RealESR_Gx4_fp16) (SRVGGNet, fp16) | 2.98 | ~6 h |
| [realesr-general-x4v3](https://huggingface.co/jonathanst29/tinier-upscale-models) (SRVGGNet, fp32) | 1.63 | ~11 h |
| [Real-ESRGAN-x4plus](https://huggingface.co/qualcomm/Real-ESRGAN-x4plus) (RRDB) | 0.09 | ~8 days |

For video, prefer compact architectures (SRVGGNet "Compact", SPAN) and fp16 exports. Many more
models are on [OpenModelDB](https://openmodeldb.info); `.pth`/`.safetensors` models can be
converted to ONNX with [chaiNNer](https://chainner.app).

## Project layout

| Folder | What |
| --- | --- |
| [`upscaler/`](upscaler/) | Rust engine: decode → ONNX Runtime → encode, previews, capabilities |
| [`gui/`](gui/) | WinUI 3 studio app (C++/WinRT, Windows App SDK 2.x, unpackaged) |
| [`build.ps1`](build.ps1) | One-step build of both, bundled together |

## Third-party software

This project doesn't bundle models or FFmpeg. Components it uses keep their own licenses:

- ONNX Runtime and DirectML (MIT), through the [`ort`](https://crates.io/crates/ort) crate (MIT/Apache-2.0)
- Windows App SDK, WinUI 3, C++/WinRT and WIL (MIT)
- FFmpeg, installed separately (LGPL or GPL, depending on the build)
- AI models chosen by the user, e.g. Real-ESRGAN weights (BSD-3-Clause)

## License

[MIT](LICENSE.md) © 2026 Lambros Vasiliou
