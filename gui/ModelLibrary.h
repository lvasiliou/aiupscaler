#pragma once

namespace AiUpscaler::ModelLibrary
{
    struct Model
    {
        std::wstring Name; // file name without extension, shown in the UI
        std::filesystem::path Path;
        uintmax_t SizeBytes{}; // including external weight files
    };

    /// %LOCALAPPDATA%\AiUpscaler\Models (created on first use).
    std::filesystem::path Folder();

    /// Models in the library folder, sorted by name.
    std::vector<Model> List();

    /// True when `path` is inside the library folder.
    bool Contains(std::filesystem::path const& path);

    /// Moves an .onnx model, and any external weight file next to it (e.g. model.data), into
    /// the library. Returns the new path. Throws std::filesystem::filesystem_error or
    /// std::runtime_error (message is user-facing) on failure.
    std::filesystem::path Import(std::filesystem::path const& source);

    /// Sends a library model and its weight files to the Recycle Bin.
    void Remove(std::filesystem::path const& model);
}
