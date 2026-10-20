#include "pch.h"
#include "Settings.h"
#include "ModelLibrary.h"

#include <fstream>
#include <sstream>

using namespace winrt::Windows::Data::Json;
namespace fs = std::filesystem;

namespace AiUpscaler
{
    namespace
    {
        std::optional<fs::path> SettingsPath()
        {
            wchar_t buffer[MAX_PATH * 4];
            DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer));
            if (length == 0 || length >= ARRAYSIZE(buffer))
            {
                return std::nullopt;
            }
            return fs::path(buffer) / L"AiUpscaler" / L"settings.json";
        }

        constexpr wchar_t ConfigurationType[] = L"ai-upscaler-configuration";

        std::optional<JsonObject> ReadJson(fs::path const& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                return std::nullopt;
            }
            std::stringstream contents;
            contents << file.rdbuf();
            JsonObject obj{ nullptr };
            if (!JsonObject::TryParse(winrt::to_hstring(contents.str()), obj))
            {
                return std::nullopt;
            }
            return obj;
        }

        std::wstring ValidFit(std::wstring fit)
        {
            return fit == L"pad" || fit == L"crop" || fit == L"stretch" ? fit : L"pad";
        }
    }

    void Settings::SaveConfiguration(fs::path const& path) const
    {
        JsonObject obj;
        obj.SetNamedValue(L"type", JsonValue::CreateStringValue(ConfigurationType));
        obj.SetNamedValue(L"version", JsonValue::CreateNumberValue(1));
        obj.SetNamedValue(L"model", JsonValue::CreateStringValue(ModelPath.empty() ? L"" : fs::path(ModelPath).filename().wstring()));
        obj.SetNamedValue(L"modelPath", JsonValue::CreateStringValue(ModelPath));
        obj.SetNamedValue(L"device", JsonValue::CreateStringValue(Device));
        obj.SetNamedValue(L"codec", JsonValue::CreateStringValue(Codec));
        obj.SetNamedValue(L"quality", JsonValue::CreateNumberValue(Quality));
        obj.SetNamedValue(L"targetHeight", JsonValue::CreateNumberValue(TargetHeight));
        obj.SetNamedValue(L"fit", JsonValue::CreateStringValue(Fit));
        obj.SetNamedValue(L"tileSize", JsonValue::CreateNumberValue(TileSize));
        obj.SetNamedValue(L"keepAudio", JsonValue::CreateBooleanValue(KeepAudio));

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << winrt::to_string(obj.Stringify());
        if (!file)
        {
            throw std::runtime_error("could not write the configuration file");
        }
    }

    bool Settings::LoadConfiguration(fs::path const& path, std::wstring& missingModel)
    {
        missingModel.clear();
        auto obj = ReadJson(path);
        if (!obj || obj->GetNamedString(L"type", L"") != ConfigurationType)
        {
            return false;
        }

        std::wstring modelPath{ obj->GetNamedString(L"modelPath", L"") };
        std::wstring modelName{ obj->GetNamedString(L"model", L"") };
        std::error_code ec;
        if (!modelPath.empty() && fs::is_regular_file(modelPath, ec))
        {
            ModelPath = modelPath;
        }
        else if (auto inLibrary = ModelLibrary::Folder() / modelName; !modelName.empty() && fs::is_regular_file(inLibrary, ec))
        {
            ModelPath = inLibrary.wstring();
        }
        else if (!modelName.empty())
        {
            missingModel = modelName; // keep the current model
        }
        else
        {
            ModelPath.clear(); // the configuration says "no model"
        }

        Device = obj->GetNamedString(L"device", Device);
        Codec = obj->GetNamedString(L"codec", Codec);
        Quality = std::clamp(static_cast<int>(obj->GetNamedNumber(L"quality", Quality)), 1, 5);
        TargetHeight = static_cast<int>(obj->GetNamedNumber(L"targetHeight", TargetHeight));
        Fit = ValidFit(std::wstring(obj->GetNamedString(L"fit", Fit)));
        TileSize = std::max(0, static_cast<int>(obj->GetNamedNumber(L"tileSize", TileSize)));
        KeepAudio = obj->GetNamedBoolean(L"keepAudio", KeepAudio);
        return true;
    }

    Settings Settings::Load()
    {
        Settings settings;
        auto path = SettingsPath();
        if (!path)
        {
            return settings;
        }

        std::ifstream file(*path, std::ios::binary);
        if (!file)
        {
            return settings;
        }
        std::stringstream contents;
        contents << file.rdbuf();

        JsonObject obj{ nullptr };
        if (!JsonObject::TryParse(winrt::to_hstring(contents.str()), obj))
        {
            return settings;
        }
        settings.ModelPath = obj.GetNamedString(L"modelPath", L"");
        settings.Device = obj.GetNamedString(L"device", settings.Device);
        settings.Codec = obj.GetNamedString(L"codec", settings.Codec);
        auto version = obj.GetNamedNumber(L"version", 1);
        settings.Quality = static_cast<int>(obj.GetNamedNumber(L"quality", settings.Quality));
        settings.TargetHeight = static_cast<int>(obj.GetNamedNumber(L"targetHeight", settings.TargetHeight));
        settings.Fit = obj.GetNamedString(L"fit", settings.Fit);
        // "No bars" made non-standard frames (e.g. 2880×2160 for 2160p); output sizes are
        // always the standard frame now.
        if (settings.Fit != L"pad" && settings.Fit != L"crop" && settings.Fit != L"stretch")
        {
            settings.Fit = L"pad";
        }
        settings.TileSize = static_cast<int>(obj.GetNamedNumber(L"tileSize", settings.TileSize));
        // Version 1 saved the old 256 tile default; auto (0) is much faster on GPUs.
        if (version < 2 && settings.TileSize == 256)
        {
            settings.TileSize = 0;
        }
        // Versions 1-2 stored quality as a raw CRF (10-35); map it onto the 1-5 scale.
        if (version < 3)
        {
            auto crf = settings.Quality;
            settings.Quality = crf <= 15 ? 5 : crf <= 18 ? 4 : crf <= 22 ? 3 : crf <= 26 ? 2 : 1;
        }
        settings.Quality = std::clamp(settings.Quality, 1, 5);
        settings.KeepAudio = obj.GetNamedBoolean(L"keepAudio", settings.KeepAudio);
        settings.AutoPreview = obj.GetNamedBoolean(L"autoPreview", settings.AutoPreview);
        settings.Theme = obj.GetNamedString(L"theme", settings.Theme);
        settings.Language = obj.GetNamedString(L"language", settings.Language);
        settings.OutputFolder = obj.GetNamedString(L"outputFolder", settings.OutputFolder);
        return settings;
    }

    int Settings::QualityToCrf(int level)
    {
        static constexpr int Crf[] = { 28, 24, 20, 17, 14 };
        return Crf[std::clamp(level, 1, 5) - 1];
    }

    void Settings::Save() const
    {
        auto path = SettingsPath();
        if (!path)
        {
            return;
        }

        JsonObject obj;
        obj.SetNamedValue(L"version", JsonValue::CreateNumberValue(3));
        obj.SetNamedValue(L"modelPath", JsonValue::CreateStringValue(ModelPath));
        obj.SetNamedValue(L"device", JsonValue::CreateStringValue(Device));
        obj.SetNamedValue(L"codec", JsonValue::CreateStringValue(Codec));
        obj.SetNamedValue(L"quality", JsonValue::CreateNumberValue(Quality));
        obj.SetNamedValue(L"targetHeight", JsonValue::CreateNumberValue(TargetHeight));
        obj.SetNamedValue(L"fit", JsonValue::CreateStringValue(Fit));
        obj.SetNamedValue(L"tileSize", JsonValue::CreateNumberValue(TileSize));
        obj.SetNamedValue(L"keepAudio", JsonValue::CreateBooleanValue(KeepAudio));
        obj.SetNamedValue(L"autoPreview", JsonValue::CreateBooleanValue(AutoPreview));
        obj.SetNamedValue(L"theme", JsonValue::CreateStringValue(Theme));
        obj.SetNamedValue(L"language", JsonValue::CreateStringValue(Language));
        obj.SetNamedValue(L"outputFolder", JsonValue::CreateStringValue(OutputFolder));

        // Settings are a convenience; failing to save them must not break the app.
        std::error_code ec;
        fs::create_directories(path->parent_path(), ec);
        std::ofstream file(*path, std::ios::binary | std::ios::trunc);
        file << winrt::to_string(obj.Stringify());
    }
}
