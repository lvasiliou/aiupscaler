#pragma once

namespace AiUpscaler::ToolLocator
{
    /// The Rust engine shipped in the app's engine\ folder.
    std::optional<std::filesystem::path> FindEngine();

    /// ffmpeg.exe / ffprobe.exe: PATH first, then winget's folders, because PATH changes
    /// from a fresh winget install only reach processes started after it.
    std::optional<std::filesystem::path> FindFfmpegTool(std::wstring const& exeName);
}
