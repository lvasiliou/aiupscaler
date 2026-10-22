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
        // Audio improvements (engine --audio-enhance); none = original audio copied bit-for-bit.
        bool AudioDialogue{ false };
        bool AudioLoudness{ false };
        bool AudioDenoise{ false };
        bool AudioUpmix{ false };

        /// Engine --audio-enhance value, e.g. "dialogue,loudness"; empty when none are on.
        std::wstring AudioEnhancements() const;
        bool AutoPreview{ true }; // AI-upscale the selected frame automatically

        // App settings (Settings page)
        std::wstring Theme{ L"system" }; // system, light, dark
        std::wstring Language;           // empty = Windows display language; else e.g. "el-GR"
        std::wstring OutputFolder;       // empty = next to the source video

        static Settings Load();
        void Save() const;

        /// Writes the upscale settings (not app settings like theme) to a configuration file
        /// the user can share or reload.
        void SaveConfiguration(std::filesystem::path const& path) const;
        /// Applies a configuration file's upscale settings on top of these. A model missing
        /// at its saved path is looked up by file name in the model library (configurations
        /// move between PCs); `missingModel` gets its file name if it isn't found there either.
        /// Returns false if the file can't be read or isn't a configuration.
        bool LoadConfiguration(std::filesystem::path const& path, std::wstring& missingModel);
        /// True when the upscale settings a configuration file holds are the same in both.
        bool SameConfiguration(Settings const& other) const;

        /// Engine --crf for a quality level (lower CRF = higher quality, bigger file).
        static int QualityToCrf(int level);
    };
}
