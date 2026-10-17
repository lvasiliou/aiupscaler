#include "pch.h"
#include "ModelLibrary.h"

#include <shellapi.h>

namespace fs = std::filesystem;

namespace AiUpscaler::ModelLibrary
{
    namespace
    {
        /// Files that may hold an ONNX model's external weights (large models keep weights
        /// outside the .onnx, e.g. Qualcomm's real_esrgan_x4plus.onnx + real_esrgan_x4plus.data).
        std::vector<fs::path> Companions(fs::path const& model)
        {
            std::vector<fs::path> files;
            auto dir = model.parent_path();
            auto stem = model.stem().wstring();
            for (auto const& name : { stem + L".data", model.filename().wstring() + L".data", stem + L".onnx_data" })
            {
                std::error_code ec;
                if (fs::is_regular_file(dir / name, ec))
                {
                    files.push_back(dir / name);
                }
            }
            return files;
        }

        void MoveFile(fs::path const& from, fs::path const& to)
        {
            // Across drives this copies then deletes the original.
            if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
            {
                throw fs::filesystem_error("move failed", from, to, std::error_code(static_cast<int>(GetLastError()), std::system_category()));
            }
        }
    }

    fs::path Folder()
    {
        wchar_t buffer[MAX_PATH * 4];
        DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer));
        auto folder = (length > 0 && length < ARRAYSIZE(buffer) ? fs::path(buffer) : fs::temp_directory_path()) / L"AiUpscaler" / L"Models";
        std::error_code ec;
        fs::create_directories(folder, ec);
        return folder;
    }

    std::vector<Model> List()
    {
        std::vector<Model> models;
        std::error_code ec;
        for (auto const& entry : fs::directory_iterator(Folder(), ec))
        {
            auto extension = entry.path().extension().wstring();
            if (!entry.is_regular_file(ec) || _wcsicmp(extension.c_str(), L".onnx") != 0)
            {
                continue;
            }
            Model model{ entry.path().stem().wstring(), entry.path(), entry.file_size(ec) };
            for (auto const& companion : Companions(entry.path()))
            {
                model.SizeBytes += fs::file_size(companion, ec);
            }
            models.push_back(std::move(model));
        }
        std::sort(models.begin(), models.end(), [](Model const& a, Model const& b) { return _wcsicmp(a.Name.c_str(), b.Name.c_str()) < 0; });
        return models;
    }

    bool Contains(fs::path const& path)
    {
        std::error_code ec;
        return !path.empty() && fs::equivalent(path.parent_path(), Folder(), ec);
    }

    fs::path Import(fs::path const& source)
    {
        auto folder = Folder();
        auto target = folder / source.filename();
        if (fs::exists(target))
        {
            throw std::runtime_error("exists");
        }
        auto companions = Companions(source);
        for (auto const& companion : companions)
        {
            if (fs::exists(folder / companion.filename()))
            {
                throw std::runtime_error("exists");
            }
        }

        // Weights first, so a model is never in the library without its weights.
        for (auto const& companion : companions)
        {
            MoveFile(companion, folder / companion.filename());
        }
        MoveFile(source, target);
        return target;
    }

    void Remove(fs::path const& model)
    {
        // SHFileOperation takes a double-null-terminated list of paths.
        std::wstring list = model.wstring() + L'\0';
        for (auto const& companion : Companions(model))
        {
            list += companion.wstring() + L'\0';
        }
        list += L'\0';

        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = list.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        if (SHFileOperationW(&operation) != 0 || operation.fAnyOperationsAborted)
        {
            throw std::runtime_error("remove failed");
        }
    }
}
