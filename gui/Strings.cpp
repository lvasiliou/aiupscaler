#include "pch.h"
#include "Strings.h"

#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/Microsoft.Windows.Globalization.h>

namespace AiUpscaler::Strings
{
    namespace
    {
        winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader& Loader()
        {
            static winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader loader;
            return loader;
        }
    }

    std::wstring Get(wchar_t const* key)
    {
        try
        {
            auto value = Loader().GetString(key);
            if (!value.empty())
            {
                return std::wstring(value);
            }
        }
        catch (winrt::hresult_error const&)
        {
        }
        return key;
    }

    std::wstring Format(wchar_t const* key, std::initializer_list<std::wstring> args)
    {
        auto text = Get(key);
        size_t index = 0;
        for (auto const& arg : args)
        {
            auto token = L"{" + std::to_wstring(index++) + L"}";
            for (size_t at = text.find(token); at != std::wstring::npos; at = text.find(token, at + arg.size()))
            {
                text.replace(at, token.size(), arg);
            }
        }
        return text;
    }

    std::vector<Language> const& Available()
    {
        static std::vector<Language> const languages{ { L"en-US", L"English" },  { L"el-GR", L"Ελληνικά" }, { L"de-DE", L"Deutsch" },
                                                      { L"fr-FR", L"Français" }, { L"zh-CN", L"简体中文" },  { L"tr-TR", L"Türkçe" },
                                                      { L"ja-JP", L"日本語" } };
        return languages;
    }

    void ApplyLanguage(std::wstring const& tag)
    {
        try
        {
            winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(tag);
        }
        catch (winrt::hresult_error const&)
        {
            // Unknown tag: keep the Windows display language.
        }
    }
}
