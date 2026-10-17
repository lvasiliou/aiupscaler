# AI Upscaler – WinUI 3 front end (C++/WinRT)

A native Windows App SDK 2.x / WinUI 3 desktop app for the Rust engine in
`../upscaler`. Unpackaged and self-contained: it runs as a plain `.exe`, with no
MSIX install and no separate Windows App Runtime installer.

## Features

**Studio layout**: the source video on the left, the AI result on the right.

- **Drop target**: drop a video on the Source pane (or its Browse button, the open icon in the
  pane header, Ctrl+O, or `AiUpscaler.exe <video>`).
- **Centred previews at the true picture shape**: both pictures are sized to the video's display
  aspect (anamorphic DVDs included) and centred in their panes at any window size.
- **Film strip**: thumbnails across the whole video on a strip of "film" with sprocket holes and a
  playhead; click a frame to jump there.
- **Timeline scrubber**: drag to any point; the Source pane shows that frame within a fraction of a second.
- **Automatic AI previews**: 3 seconds after the frame last changed (timeline, film strip, new
  model or device), the frame is AI-upscaled and shown on the right. Waiting for you to settle
  avoids running the GPU for every position passed during a long drag. The **Generate AI
  previews** tickbox under the preview turns this off; Ctrl+P previews the current frame anytime.
- **Synced zoom and pan**: zoom (Ctrl+wheel or pinch) or scroll either pane and the other follows,
  so the same detail lines up side by side. One **Fit / 1:1** toggle switches between fitting the
  frame and 100% pixels.
- **Live previews while upscaling**: a source/output pair from the frame being processed every
  ~2 s, with the timeline following progress.
- **Preview runs**: the first 10/30/60 s only (saved as `*_preview.mp4`, audio trimmed to match).
- **Upscale settings tab** (first): AI model (by name, from the model library), output path, AI
  device, output size, picture shape, encoder, quality (1–5), tile size, audio.
  - **Output size** always produces the standard 16:9 frame: 720p 1280×720, 1080p 1920×1080,
    1440p 2560×1440, 4K 3840×2160. **Picture shape** decides how a picture of another shape (e.g.
    4:3) fills it: black bars (default), crop, or stretch. The resulting file size is shown under
    *Output file* and in the toolbar summary.
  - **Hardware-aware options**: at startup the engine checks which AI devices and video encoders
    really work on this PC; unsupported ones are greyed out with a tooltip saying why.
  - **Quality 1–5** maps to the encoder's constant quality (CRF 28, 24, 20, 17, 14).
- **Progress tab**: frame count, fps, and a time estimate with the local finish time, measured
  after warm-up. Previews also estimate the whole video. Cancel stops cleanly and deletes the
  partial file. When a file or preview is ready, **Open** plays it in the default player. The
  engine **Log** is a collapsible section here (it opens by itself when a run fails).
- **Settings** (gear button, Esc to go back):
  - **Appearance**: theme (Windows setting, light, dark) and language (Windows default, English,
    Ελληνικά; applies after a restart, offered right there).
  - **Output**: default output folder: next to the source video, or a folder you choose.
  - **Models**: the model library in `%LOCALAPPDATA%\AiUpscaler\Models`. **Import** *moves* an
    `.onnx` file (and any external weights file next to it, e.g. `model.data`) into the library;
    the trash button sends a model to the Recycle Bin.
- Settings are remembered in `%LOCALAPPDATA%\AiUpscaler\settings.json`.
- Mica backdrop and a custom title bar; in light theme the caption buttons use the accent colour.

## Languages

UI text lives in `Strings\<language>\Resources.resw` (MRT Core), with `x:Uid` in XAML and
`Strings::Get`/`Format` in code. Both languages are generated from one table:
edit `Strings\strings.tsv` (key, English, Greek), then run `Strings\make-resw.ps1`. To add a
language, add a column for it in the table and generator, add `PRIResource` for the new
`.resw` in the project, and list it in `Strings::Available()`.

## How it talks to the engine

`EngineJob` starts `engine\ai-upscaler.exe` with redirected pipes:

- **Upscale runs** use `--json --preview-dir <temp> --preview-interval 2`.
- **Scrubbing** runs `--frame-at <secs> --source-only`, and AI previews run `--frame-at <secs>` with the model.
  These are one-shot jobs, one at a time; a newer request replaces one still waiting.
- **Opening a video** runs `--probe` for size, frame rate and duration.
- **stdout**: JSON lines (`probe`, `start`, `progress`, `preview`, `done`, `cancelled`)
- **stderr**: human-readable log, shown in the log panel
- **stdin**: writing `cancel` stops the job; if stdin closes, the engine also stops

The engine runs inside a kill-on-close **Job object**, so it and every ffmpeg
process it starts are terminated if the app closes or crashes.

## Build

From the repository root:

```powershell
.\build.ps1
```

This builds the Rust engine, downloads the NuGet packages (`restore-packages.ps1`,
no nuget.exe needed) and builds the app with MSBuild. Requirements: Visual Studio
2022/2026 with the C++ desktop workload, a Windows 10/11 SDK, Rust, and ffmpeg on
PATH (`winget install Gyan.FFmpeg`).

Builds go to `%LOCALAPPDATA%\AiUpscalerBuild` by default: a short path, because MIDL
and the linker break on paths over 260 characters, and outside `%TEMP%`, where MSBuild's
incremental C++ builds are unreliable. Use `-BuildRoot` to choose another short folder.

`.\build.ps1 -Cuda` builds a variant with the NVIDIA CUDA backend into `gui-cuda\bin`
(it needs CUDA 13.x and cuDNN 9 on the running PC; see the main README).

## Files

| File | Purpose |
| --- | --- |
| `MainWindow.xaml` / `.xaml.cpp` | UI and event handlers |
| `EngineJob.h/.cpp` | Child process, pipes, Job object, JSON event parsing |
| `ToolLocator.h/.cpp` | Finds the bundled engine and ffmpeg/ffprobe (PATH, then winget) |
| `Settings.h/.cpp` | Persisted user settings |
| `ModelLibrary.h/.cpp` | Model library folder: list, import (move), remove (Recycle Bin) |
| `Strings.h/.cpp`, `Strings\` | Localized text and UI language selection |
| `AiUpscaler.vcxproj`, `packages.config` | Hand-written project; imports each Windows App SDK sub-package explicitly (`packages.config` doesn't resolve the 2.x metapackage's dependencies) |
