#pragma once

/// Localized UI strings from Strings\<language>\Resources.resw (MRT Core).
/// XAML elements use x:Uid; code uses these helpers with plain keys.
namespace AiUpscaler::Strings
{
    /// The string for `key` in the current UI language (the key itself if missing).
    std::wstring Get(wchar_t const* key);

    /// Get(key) with {0}, {1}, ... replaced by `args`.
    std::wstring Format(wchar_t const* key, std::initializer_list<std::wstring> args);

    /// Languages the app ships, as (tag, native name); the first entry is the default.
    struct Language
    {
        wchar_t const* Tag;
        wchar_t const* NativeName;
    };
    std::vector<Language> const& Available();

    /// Applies a saved UI language (empty = Windows display language). Must run before any
    /// window or resource is created; changing it later needs an app restart.
    void ApplyLanguage(std::wstring const& tag);
}
