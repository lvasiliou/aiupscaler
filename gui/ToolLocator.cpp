#include "pch.h"
#include "ToolLocator.h"

namespace fs = std::filesystem;

namespace AiUpscaler::ToolLocator
{
    namespace
    {
        fs::path ExeDirectory()
        {
            std::wstring buffer(MAX_PATH, L'\0');
            for (;;)
            {
                DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length < buffer.size())
                {
                    buffer.resize(length);
                    return fs::path(buffer).parent_path();
                }
                buffer.resize(buffer.size() * 2);
            }
        }

        std::optional<fs::path> FindOnPath(std::wstring const& exeName)
        {
            wchar_t found[MAX_PATH * 4];
            DWORD length = SearchPathW(nullptr, exeName.c_str(), nullptr, ARRAYSIZE(found), found, nullptr);
            if (length > 0 && length < ARRAYSIZE(found))
            {
                return fs::path(found);
            }
            return std::nullopt;
        }

        std::optional<fs::path> LocalAppData()
        {
            wchar_t buffer[MAX_PATH * 4];
            DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer));
            if (length == 0 || length >= ARRAYSIZE(buffer))
            {
                return std::nullopt;
            }
            return fs::path(buffer);
        }
    }

    std::optional<fs::path> FindEngine()
    {
        auto dir = ExeDirectory();
        for (auto const& candidate : { dir / L"engine" / L"ai-upscaler.exe", dir / L"ai-upscaler.exe" })
        {
            if (fs::exists(candidate))
            {
                return candidate;
            }
        }
        return FindOnPath(L"ai-upscaler.exe");
    }

    std::optional<fs::path> FindFfmpegTool(std::wstring const& exeName)
    {
        if (auto onPath = FindOnPath(exeName))
        {
            return onPath;
        }

        auto localAppData = LocalAppData();
        if (!localAppData)
        {
            return std::nullopt;
        }
        auto winget = *localAppData / L"Microsoft" / L"WinGet";
        if (auto link = winget / L"Links" / exeName; fs::exists(link))
        {
            return link;
        }

        std::error_code ec;
        for (auto const& package : fs::directory_iterator(winget / L"Packages", ec))
        {
            if (package.path().filename().wstring().find(L"FFmpeg") == std::wstring::npos)
            {
                continue;
            }
            for (auto const& entry : fs::recursive_directory_iterator(package.path(), ec))
            {
                if (entry.path().filename() == exeName)
                {
                    return entry.path();
                }
            }
        }
        return std::nullopt;
    }
}
