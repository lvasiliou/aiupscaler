#pragma once

namespace AiUpscaler
{
    /// User choices remembered between runs, stored as JSON in %LOCALAPPDATA%\AiUpscaler.
    struct Settings
    {
        std::wstring ModelPath;
        std::wstring Device{ L"auto" };
        std::wstring Codec{ L"auto" };
        int Quality{ 4 }; // 1 (smallest file) .. 5 (best); see QualityToCrf
        int TargetHeight{ 0 }; // 0 = native (model scale); else a 16:9 frame of this height
        std::wstring Fit{ L"pad" }; // engine --fit: pad, crop, stretch (frames are always the standard size)
        int TileSize{ 0 }; // 0 = auto
        bool KeepAudio{ true };
        bool AutoPreview{ true }; // AI-upscale the selected frame automatically

        // App settings (Settings page)
        std::wstring Theme{ L"system" }; // system, light, dark
        std::wstring Language;           // empty = Windows display language; else e.g. "el-GR"
        std::wstring OutputFolder;       // empty = next to the source video

        static Settings Load();
        void Save() const;

        /// Engine --crf for a quality level (lower CRF = higher quality, bigger file).
        static int QualityToCrf(int level);
    };
}
