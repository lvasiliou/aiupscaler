#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "ModelLibrary.h"
#include "Strings.h"
#include "ToolLocator.h"

#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::Windows::Storage::Pickers;
using winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation;
using winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats;
using winrt::Windows::Foundation::IAsyncOperation;
namespace fs = std::filesystem;

namespace
{
    /// Localized string.
    std::wstring T(wchar_t const* key)
    {
        return ::AiUpscaler::Strings::Get(key);
    }

    /// Localized template with {0}, {1}, ... filled in.
    std::wstring F(wchar_t const* key, std::initializer_list<std::wstring> args)
    {
        return ::AiUpscaler::Strings::Format(key, args);
    }

    std::wstring Num(double value, int decimals)
    {
        wchar_t buffer[32];
        swprintf_s(buffer, L"%.*f", decimals, value);
        return buffer;
    }

    std::wstring FormatDuration(double seconds)
    {
        auto total = static_cast<long long>(seconds + 0.5);
        wchar_t buffer[32];
        if (total >= 3600)
        {
            swprintf_s(buffer, L"%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
        }
        else
        {
            swprintf_s(buffer, L"%lld:%02lld", total / 60, total % 60);
        }
        return buffer;
    }

    /// "14 h 32 min", "5 min 20 s": friendlier than h:mm:ss for long estimates.
    std::wstring FormatLongDuration(double seconds)
    {
        auto total = static_cast<long long>(seconds + 0.5);
        if (total >= 86400)
        {
            return F(L"Dur_Days", { std::to_wstring(total / 86400), std::to_wstring((total / 3600) % 24) });
        }
        if (total >= 3600)
        {
            return F(L"Dur_Hours", { std::to_wstring(total / 3600), std::to_wstring((total / 60) % 60) });
        }
        if (total >= 60)
        {
            return F(L"Dur_Minutes", { std::to_wstring(total / 60), std::to_wstring(total % 60) });
        }
        return F(L"Dur_Seconds", { std::to_wstring(total) });
    }

    /// Local clock time `seconds` from now, e.g. "today 23:40" or "Sat 18 Oct 03:15".
    std::wstring FinishTime(double seconds)
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        FILETIME fileTime{};
        SystemTimeToFileTime(&now, &fileTime);
        ULARGE_INTEGER ticks{};
        ticks.LowPart = fileTime.dwLowDateTime;
        ticks.HighPart = fileTime.dwHighDateTime;
        ticks.QuadPart += static_cast<ULONGLONG>(seconds * 10'000'000.0);
        fileTime.dwLowDateTime = ticks.LowPart;
        fileTime.dwHighDateTime = ticks.HighPart;
        SYSTEMTIME end{};
        FileTimeToSystemTime(&fileTime, &end);

        wchar_t time[64]{};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &end, nullptr, time, ARRAYSIZE(time));
        if (end.wYear == now.wYear && end.wMonth == now.wMonth && end.wDay == now.wDay)
        {
            return F(L"Time_Today", { time });
        }
        wchar_t date[64]{};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &end, L"ddd d MMM", date, ARRAYSIZE(date), nullptr);
        return std::wstring(date) + L" " + time;
    }

    /// "4:3", "16:9", or "1.85:1" for a display aspect ratio.
    std::wstring AspectName(double aspect)
    {
        struct Named { double Value; wchar_t const* Name; };
        static constexpr Named Known[] = { { 4.0 / 3.0, L"4:3" }, { 16.0 / 9.0, L"16:9" }, { 1.85, L"1.85:1" }, { 2.39, L"2.39:1" },
                                           { 3.0 / 2.0, L"3:2" }, { 5.0 / 4.0, L"5:4" }, { 1.0, L"1:1" } };
        for (auto const& known : Known)
        {
            if (std::abs(aspect - known.Value) < 0.02)
            {
                return known.Name;
            }
        }
        return Num(aspect, 2) + L":1";
    }

    std::wstring FileSize(uintmax_t bytes)
    {
        return F(L"Size_MB", { Num(bytes / 1048576.0, bytes < 10 * 1048576 ? 1 : 0) });
    }

    int Even(double value)
    {
        return std::max(2, static_cast<int>(std::lround(value / 2.0)) * 2);
    }

    /// Standard 16:9 frame width for a height (720 -> 1280, 1080 -> 1920, ...).
    int FrameWidthFor(int height)
    {
        return Even(height * 16.0 / 9.0);
    }

    std::wstring PreviewPathFor(std::wstring const& output)
    {
        fs::path path(output);
        return (path.parent_path() / (path.stem().wstring() + L"_preview" + path.extension().wstring())).wstring();
    }

    void SelectByTag(ComboBox const& combo, std::wstring const& tag)
    {
        for (uint32_t i = 0; i < combo.Items().Size(); ++i)
        {
            auto item = combo.Items().GetAt(i).as<ComboBoxItem>();
            if (unbox_value<hstring>(item.Tag()) == tag)
            {
                combo.SelectedIndex(static_cast<int32_t>(i));
                return;
            }
        }
        combo.SelectedIndex(0);
    }

    std::wstring SelectedTag(ComboBox const& combo)
    {
        auto item = combo.SelectedItem().try_as<ComboBoxItem>();
        return item ? std::wstring(unbox_value_or<hstring>(item.Tag(), L"")) : std::wstring{};
    }

    ComboBoxItem MakeItem(std::wstring const& content, std::wstring const& tag)
    {
        ComboBoxItem item;
        item.Content(box_value(content));
        item.Tag(box_value(tag));
        return item;
    }

    std::wstring FormatSeconds(double seconds)
    {
        return Num(seconds, 3);
    }

    fs::path MakePreviewDir()
    {
        wchar_t temp[MAX_PATH + 1]{};
        GetTempPathW(ARRAYSIZE(temp), temp);
        // Per-process so two app windows never delete each other's images.
        auto dir = fs::path(temp) / L"AiUpscaler" / std::to_wstring(GetCurrentProcessId());
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }

    constexpr wchar_t const* VideoExtensions[] = { L".mp4", L".mkv", L".mov", L".avi", L".webm", L".m4v", L".wmv", L".ts", L".flv", L".mpg", L".vob" };

    std::wstring LowerExtension(std::wstring const& path)
    {
        auto extension = fs::path(path).extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        return extension;
    }

    bool IsConfigFile(std::wstring const& path)
    {
        return LowerExtension(path) == L".json"; // configuration files (Settings::SaveConfiguration)
    }

    bool IsVideoFile(std::wstring const& path)
    {
        auto extension = LowerExtension(path);
        return std::any_of(std::begin(VideoExtensions), std::end(VideoExtensions), [&](auto known) { return extension == known; });
    }

    bool SamePath(std::wstring const& a, std::wstring const& b)
    {
        std::error_code ec;
        return _wcsicmp(a.c_str(), b.c_str()) == 0 || fs::equivalent(a, b, ec);
    }
}

namespace winrt::AiUpscaler::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        InitializeWindow();

        m_appSettings = ::AiUpscaler::Settings::Load();
        m_startupLanguage = m_appSettings.Language;
        ApplyTheme(m_appSettings.Theme);

        // Re-tint the caption buttons when Windows switches light/dark ("Use Windows setting")
        // or the user picks a new accent colour.
        RootGrid().ActualThemeChanged([this](auto&&, auto&&) { UpdateCaptionButtons(); });
        RootGrid().Loaded([this](auto&&, auto&&) { UpdateCaptionButtons(); });
        m_colorValuesChanged = m_uiSettings.ColorValuesChanged(winrt::auto_revoke, [weak = get_weak(), dispatcher = DispatcherQueue()](auto&&, auto&&)
        {
            // Raised on a background thread.
            dispatcher.TryEnqueue([weak]
            {
                if (auto self = weak.get())
                {
                    self->UpdateCaptionButtons();
                }
            });
        });
        ApplySettings(m_appSettings);
        m_previewDir = MakePreviewDir();

        StatusText().Text(T(L"Status_Ready"));
        OutputPlaceholderText().Text(T(L"Placeholder_Open"));
        ShowZoomState(false);

        // Scrubbing is debounced so dragging the timeline doesn't start an engine per pixel.
        m_scrubTimer = DispatcherQueue().CreateTimer();
        m_scrubTimer.Interval(std::chrono::milliseconds(150));
        m_scrubTimer.IsRepeating(false);
        m_scrubTimer.Tick([this](auto&&, auto&&) { RequestSourceFrame(TimelineSlider().Value()); });

        m_aiPreviewTimer = DispatcherQueue().CreateTimer();
        m_aiPreviewTimer.Interval(std::chrono::seconds(3));
        m_aiPreviewTimer.IsRepeating(false);
        m_aiPreviewTimer.Tick([this](auto&&, auto&&) { RequestAiPreview(); });

        UpdateOutputSummary();
        StartCapabilities();

        // The settings the app starts with are the baseline for "unsaved changes".
        m_savedConfig = CollectSettings();
        m_configTracking = true;
        UpdateConfigTitle();
        RefreshBatchList();

        // Closing with unsaved config changes asks to save them first.
        AppWindow().Closing([this](auto&&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& e)
        {
            if (m_closeConfirmed || !HasUnsavedChanges())
            {
                return;
            }
            e.Cancel(true);
            CloseAfterConfirm();
        });

        Closed([this](auto&&, auto&&)
        {
            // Destroying a job cancels the engine; the Job object guarantees cleanup.
            for (auto* job : { &m_previewJob, &m_thumbJob, &m_capsJob })
            {
                if (*job)
                {
                    (*job)->Terminate();
                }
                job->reset();
            }
            m_job.reset();
            std::error_code ec;
            fs::remove_all(m_previewDir, ec);
        });
    }

    void MainWindow::InitializeWindow()
    {
        SystemBackdrop(Media::MicaBackdrop());
        ExtendsContentIntoTitleBar(true);
        SetTitleBar(AppTitleBar());
        AppWindow().TitleBar().PreferredHeightOption(Microsoft::UI::Windowing::TitleBarHeightOption::Tall);

        // The app icon (resource 1, AiUpscaler.rc) for the taskbar and Alt+Tab; the title bar
        // shows the same icon from Assets\AppIcon.png next to the .exe.
        if (auto icon = static_cast<HICON>(::LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED)))
        {
            AppWindow().SetIcon(Microsoft::UI::GetIconIdFromIcon(icon));
        }
        wchar_t exe[MAX_PATH * 4]{};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        auto iconPng = fs::path(exe).parent_path() / L"Assets" / L"AppIcon.png";
        Media::Imaging::BitmapImage iconImage;
        iconImage.DecodePixelWidth(64); // sharp at 18 px up to 350% scaling
        iconImage.UriSource(Windows::Foundation::Uri(iconPng.wstring()));
        AppIconImage().Source(iconImage);

        auto hwnd = Microsoft::UI::GetWindowFromWindowId(AppWindow().Id());
        double scale = GetDpiForWindow(hwnd) / 96.0;
        AppWindow().Resize({ static_cast<int32_t>(1440 * scale), static_cast<int32_t>(960 * scale) });
    }

    void MainWindow::ApplySettings(::AiUpscaler::Settings const& settings)
    {
        RefreshModels(settings.ModelPath);
        SelectByTag(DeviceCombo(), settings.Device);
        SelectByTag(CodecCombo(), settings.Codec);
        SelectByTag(HeightCombo(), std::to_wstring(settings.TargetHeight));
        SelectByTag(FitCombo(), settings.Fit);
        SelectByTag(FormatCombo(), settings.Container);
        FitCombo().IsEnabled(settings.TargetHeight > 0);
        QualitySlider().Value(settings.Quality);
        TileBox().Value(settings.TileSize);
        // Toggling the switch saves settings; don't save half-applied ones.
        m_applyingSettings = true;
        AudioToggle().IsOn(settings.KeepAudio);
        AudioDialogueItem().IsChecked(settings.AudioDialogue);
        AudioLoudnessItem().IsChecked(settings.AudioLoudness);
        AudioDenoiseItem().IsChecked(settings.AudioDenoise);
        AudioUpmixItem().IsChecked(settings.AudioUpmix);
        m_applyingSettings = false;
        UpdateAudioButton();
        AutoPreviewCheck().IsChecked(settings.AutoPreview);
    }

    void MainWindow::OnAudioOptionChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (!AudioImproveButton() || m_applyingSettings)
        {
            return;
        }
        UpdateAudioButton();
        CollectSettings().Save();
        UpdateConfigTitle();
    }

    void MainWindow::UpdateAudioButton()
    {
        // e.g. "Original audio" or "Improved: dialogue, volume"
        std::wstring summary;
        for (auto [item, key] : { std::pair{ AudioDialogueItem(), L"Audio_ShortDialogue" }, std::pair{ AudioLoudnessItem(), L"Audio_ShortLoudness" },
                                  std::pair{ AudioDenoiseItem(), L"Audio_ShortDenoise" }, std::pair{ AudioUpmixItem(), L"Audio_ShortUpmix" } })
        {
            if (item.IsChecked())
            {
                summary += (summary.empty() ? L"" : L", ") + T(key);
            }
        }
        AudioImproveButton().Content(box_value(summary.empty() ? T(L"Audio_Original") : F(L"Audio_ImprovedSummary", { summary })));
        AudioImproveButton().IsEnabled(AudioToggle().IsOn() && !(m_job && m_job->IsRunning()));
    }

    ::AiUpscaler::Settings MainWindow::CollectSettings()
    {
        // Start from the app-level settings so saving never drops theme, language or folder.
        ::AiUpscaler::Settings settings = m_appSettings;
        settings.ModelPath = SelectedTag(ModelCombo());
        settings.Device = SelectedTag(DeviceCombo());
        settings.Codec = SelectedTag(CodecCombo());
        settings.TargetHeight = _wtoi(SelectedTag(HeightCombo()).c_str());
        settings.Fit = SelectedTag(FitCombo());
        settings.Quality = static_cast<int>(QualitySlider().Value());
        auto tile = TileBox().Value();
        settings.TileSize = std::isnan(tile) ? 0 : static_cast<int>(tile);
        settings.KeepAudio = AudioToggle().IsOn();
        settings.Container = OutputContainer();
        settings.AudioDialogue = AudioDialogueItem().IsChecked();
        settings.AudioLoudness = AudioLoudnessItem().IsChecked();
        settings.AudioDenoise = AudioDenoiseItem().IsChecked();
        settings.AudioUpmix = AudioUpmixItem().IsChecked();
        settings.AutoPreview = unbox_value_or<bool>(AutoPreviewCheck().IsChecked(), false);
        return settings;
    }

    std::optional<MainWindow::Tools> MainWindow::FindTools()
    {
        auto engine = ::AiUpscaler::ToolLocator::FindEngine();
        auto ffmpeg = ::AiUpscaler::ToolLocator::FindFfmpegTool(L"ffmpeg.exe");
        auto ffprobe = ::AiUpscaler::ToolLocator::FindFfmpegTool(L"ffprobe.exe");
        if (!engine)
        {
            ShowResult(InfoBarSeverity::Error, T(L"Err_EngineMissing_Title"), T(L"Err_EngineMissing"), false);
            return std::nullopt;
        }
        if (!ffmpeg || !ffprobe)
        {
            ShowResult(InfoBarSeverity::Error, T(L"Err_FfmpegMissing_Title"), T(L"Err_FfmpegMissing"), false);
            return std::nullopt;
        }
        return Tools{ *engine, *ffmpeg, *ffprobe };
    }

    std::wstring MainWindow::DefaultOutputFor(std::wstring const& input)
    {
        fs::path source(input);
        auto name = source.stem().wstring() + L"_upscaled." + OutputContainer();
        std::error_code ec;
        if (!m_appSettings.OutputFolder.empty() && fs::is_directory(m_appSettings.OutputFolder, ec))
        {
            return (fs::path(m_appSettings.OutputFolder) / name).wstring();
        }
        return (source.parent_path() / name).wstring();
    }

    // ---- Source selection ----------------------------------------------------------------

    void MainWindow::OpenFile(std::wstring const& path)
    {
        if (fs::exists(path))
        {
            if (IsConfigFile(path))
            {
                LoadConfigurationFile(path);
            }
            else
            {
                SetInput(path);
            }
        }
        else
        {
            ShowResult(InfoBarSeverity::Warning, T(L"Err_FileNotFound_Title"), path, false);
        }
    }

    void MainWindow::SetInput(std::wstring const& path)
    {
        m_inputPath = path;
        SourceNameText().Text(fs::path(path).filename().wstring());
        SourceInfoText().Text(T(L"Source_Reading"));

        // Keep the output path in sync until the user picks one themselves.
        std::wstring output{ OutputBox().Text() };
        if (output.empty() || output == m_autoOutput)
        {
            m_autoOutput = DefaultOutputFor(path);
            OutputBox().Text(m_autoOutput);
        }

        SourceImage().Source(nullptr);
        OutputImage().Source(nullptr);
        OutputInfoText().Text(L"");
        OutputPlaceholder().Visibility(Visibility::Visible);
        OutputPlaceholderText().Text(T(L"Placeholder_PickFrame"));
        SourceDropZone().Visibility(Visibility::Collapsed);
        OpenSourceButton().Visibility(Visibility::Visible);
        TimelineSlider().IsEnabled(false);
        m_aiPreviewTimer.Stop();
        AutoPreviewStatus().Text(L"");
        m_duration = 0;
        m_sourceWidth = m_sourceHeight = 0;
        m_sar = 1.0;
        ResultBar().IsOpen(false);

        // A new video gets a new film strip.
        if (m_thumbJob)
        {
            m_thumbJob->Terminate();
            m_thumbJob.reset();
        }
        m_thumbTimes.clear();
        FilmFrames().Children().Clear();
        FilmFrames().ColumnDefinitions().Clear();
        FilmStrip().Visibility(Visibility::Collapsed);
        UpdateOutputSummary();

        auto tools = FindTools();
        if (!tools)
        {
            return;
        }
        StartPreviewJob({ path, L"--probe", L"--ffprobe", tools->Ffprobe.wstring() }, false);
    }

    fire_and_forget MainWindow::OnBrowseInput(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        FileOpenPicker picker(AppWindow().Id());
        picker.SuggestedStartLocation(PickerLocationId::VideosLibrary);
        for (auto extension : VideoExtensions)
        {
            picker.FileTypeFilter().Append(extension);
        }
        if (auto result = co_await picker.PickSingleFileAsync())
        {
            SetInput(std::wstring(result.Path()));
        }
    }

    fire_and_forget MainWindow::OnBrowseOutput(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        FileSavePicker picker(AppWindow().Id());
        picker.SuggestedStartLocation(PickerLocationId::VideosLibrary);
        picker.FileTypeChoices().Insert(T(L"Picker_Mp4"), single_threaded_vector<hstring>({ L".mp4" }));
        picker.FileTypeChoices().Insert(T(L"Picker_Mkv"), single_threaded_vector<hstring>({ L".mkv" }));
        picker.FileTypeChoices().Insert(T(L"Picker_Mov"), single_threaded_vector<hstring>({ L".mov" }));
        std::wstring current{ OutputBox().Text() };
        picker.SuggestedFileName(current.empty() ? T(L"Picker_DefaultName") : fs::path(current).stem().wstring());
        if (auto result = co_await picker.PickSaveFileAsync())
        {
            std::wstring path{ result.Path() };
            OutputBox().Text(path);
            // Picking an .mp4 or .mkv file also picks that format (an .mov keeps the setting).
            auto extension = LowerExtension(path);
            if (extension == L".mp4" || extension == L".mkv")
            {
                m_syncingFormat = true;
                SelectByTag(FormatCombo(), extension.substr(1));
                m_syncingFormat = false;
            }
        }
    }

    std::wstring MainWindow::OutputContainer()
    {
        auto container = FormatCombo() ? SelectedTag(FormatCombo()) : L"";
        return container.empty() ? L"mp4" : container;
    }

    fire_and_forget MainWindow::OnSaveConfiguration(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        co_await SaveConfigAsync(false);
    }

    fire_and_forget MainWindow::OnSaveConfigurationAs(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        co_await SaveConfigAsync(true);
    }

    fire_and_forget MainWindow::OnLoadConfiguration(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        if ((m_job && m_job->IsRunning()) || !co_await ConfirmUnsavedChangesAsync())
        {
            co_return;
        }
        FileOpenPicker picker(AppWindow().Id());
        picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
        picker.FileTypeFilter().Append(L".json");
        auto result = co_await picker.PickSingleFileAsync();
        if (!result || (m_job && m_job->IsRunning()))
        {
            co_return;
        }
        LoadConfigurationFile(std::wstring(result.Path()));
    }

    fire_and_forget MainWindow::LoadConfigurationAfterConfirm(std::wstring path)
    {
        auto lifetime = get_strong();
        if (!(m_job && m_job->IsRunning()) && co_await ConfirmUnsavedChangesAsync())
        {
            LoadConfigurationFile(path);
        }
    }

    void MainWindow::LoadConfigurationFile(fs::path const& path)
    {
        auto settings = CollectSettings();
        std::wstring missingModel;
        if (!settings.LoadConfiguration(path, missingModel))
        {
            ShowConfigMessage(InfoBarSeverity::Error, T(L"Config_LoadFailed"), F(L"Config_NotConfig", { path.filename().wstring() }));
            return;
        }

        ApplySettings(settings);
        // A configuration from another PC may pick a device or encoder this one can't use.
        for (auto const& combo : { DeviceCombo(), CodecCombo() })
        {
            auto selected = combo.SelectedItem().try_as<ComboBoxItem>();
            for (uint32_t i = 0; selected && !selected.IsEnabled() && i < combo.Items().Size(); ++i)
            {
                if (combo.Items().GetAt(i).as<ComboBoxItem>().IsEnabled())
                {
                    combo.SelectedIndex(static_cast<int32_t>(i));
                    break;
                }
            }
        }
        CollectSettings().Save();
        m_modelScale = 0;
        UpdateOutputSummary();
        ScheduleAiPreview();
        ShowBottomTab(UpscaleSettingsTab());

        // Saving goes back to this file from now on. The baseline is what the controls now hold,
        // so options this PC lacks (or a missing model) don't count as unsaved changes.
        m_configPath = path.wstring();
        m_savedConfig = CollectSettings();
        UpdateConfigTitle();

        if (missingModel.empty())
        {
            ShowConfigMessage(InfoBarSeverity::Success, T(L"Config_Loaded"), path.filename().wstring());
        }
        else
        {
            ShowConfigMessage(InfoBarSeverity::Warning, T(L"Config_Loaded"), F(L"Config_ModelMissing", { missingModel }));
        }
    }

    void MainWindow::ShowConfigMessage(InfoBarSeverity severity, std::wstring const& title, std::wstring const& message)
    {
        ConfigBar().Severity(severity);
        ConfigBar().Title(title);
        ConfigBar().Message(message);
        ConfigBar().IsOpen(true);

        // Confirmations go away by themselves; problems stay until closed.
        if (!m_configBarTimer)
        {
            m_configBarTimer = DispatcherQueue().CreateTimer();
            m_configBarTimer.Interval(std::chrono::seconds(5));
            m_configBarTimer.IsRepeating(false);
            m_configBarTimer.Tick([this](auto&&, auto&&) { ConfigBar().IsOpen(false); });
        }
        m_configBarTimer.Stop();
        if (severity == InfoBarSeverity::Success)
        {
            m_configBarTimer.Start();
        }
    }

    void MainWindow::OnManageModels(IInspectable const&, RoutedEventArgs const&)
    {
        OnOpenAppSettings(nullptr, nullptr);
        SettingsNavList().SelectedItem(ModelsNavItem());
    }

    void MainWindow::OnQualityChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        // Fires during InitializeComponent (Minimum coerces Value) before QualityText exists.
        if (!QualityText())
        {
            return;
        }
        auto level = std::clamp(static_cast<int>(std::lround(e.NewValue())), 1, 5);
        std::wstring label = T((L"Quality_" + std::to_wstring(level)).c_str());
        QualityText().Text(F(L"Quality_Format", { std::to_wstring(level), label }));
        UpdateOutputSummary();
        UpdateConfigTitle();
    }

    void MainWindow::OnTileChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&)
    {
        UpdateConfigTitle();
    }

    void MainWindow::OnSettingChanged(IInspectable const& sender, RoutedEventArgs const&)
    {
        // Fires during InitializeComponent/ApplySettings before every control exists.
        if (!FinalSizeText() || !FitCombo() || m_refreshingModels || m_refreshingItems)
        {
            return;
        }
        if (sender == ModelCombo())
        {
            m_modelScale = 0; // a different model may have a different scale
        }
        if (sender == FormatCombo() && !m_syncingFormat)
        {
            // The output file follows the format, whether it was chosen automatically or by hand.
            std::wstring output{ OutputBox().Text() };
            if (!output.empty())
            {
                auto changed = fs::path(output).replace_extension(L"." + OutputContainer()).wstring();
                if (output == m_autoOutput)
                {
                    m_autoOutput = changed;
                }
                OutputBox().Text(changed);
            }
        }
        FitCombo().IsEnabled(SelectedTag(HeightCombo()) != L"0" && !(m_job && m_job->IsRunning()));
        UpdateOutputSummary();
        UpdateConfigTitle();
        if (sender == ModelCombo() || sender == DeviceCombo())
        {
            ScheduleAiPreview(); // show the frame with the new model or device
        }
    }

    void MainWindow::OnOpenAccelerator(Input::KeyboardAccelerator const&, Input::KeyboardAcceleratorInvokedEventArgs const& e)
    {
        e.Handled(true);
        if (!(m_job && m_job->IsRunning()) && AppSettingsView().Visibility() == Visibility::Collapsed)
        {
            OnBrowseInput(nullptr, nullptr);
        }
    }

    // ---- Drop target (source pane) ---------------------------------------------------------

    void MainWindow::OnSourceDragEnter(IInspectable const&, DragEventArgs const& e)
    {
        if (e.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            SourceDropHighlight().Visibility(Visibility::Visible);
        }
    }

    void MainWindow::OnSourceDragOver(IInspectable const&, DragEventArgs const& e)
    {
        if (e.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            e.AcceptedOperation(DataPackageOperation::Copy);
            e.DragUIOverride().Caption(T(L"Drag_OpenAsSource"));
        }
    }

    void MainWindow::OnSourceDragLeave(IInspectable const&, DragEventArgs const&)
    {
        SourceDropHighlight().Visibility(Visibility::Collapsed);
    }

    fire_and_forget MainWindow::OnSourceDrop(IInspectable const&, DragEventArgs const& e)
    {
        auto lifetime = get_strong();
        SourceDropHighlight().Visibility(Visibility::Collapsed);
        auto view = e.DataView();
        if (!view.Contains(StandardDataFormats::StorageItems()))
        {
            co_return;
        }
        auto items = co_await view.GetStorageItemsAsync();
        for (auto const& item : items)
        {
            if (!item.IsOfType(Windows::Storage::StorageItemTypes::File))
            {
                continue;
            }
            std::wstring path{ item.Path() };
            if (IsConfigFile(path))
            {
                LoadConfigurationAfterConfirm(path);
                break;
            }
            if (LowerExtension(path) != L".onnx")
            {
                SetInput(path);
                break;
            }
        }
    }

    // ---- Probe and frame previews ------------------------------------------------------------

    void MainWindow::StartPreviewJob(std::vector<std::wstring> args, bool aiFrame)
    {
        // The current job stays in m_previewJob until its exit is handled on this thread,
        // so "non-null" means busy; queue the newest request and drop older pending ones.
        if (m_previewJob)
        {
            m_pendingPreviewArgs = std::move(args);
            m_pendingPreviewIsAi = aiFrame;
            return;
        }
        auto tools = FindTools();
        if (!tools)
        {
            return;
        }

        auto onUi = [weak = get_weak(), dispatcher = DispatcherQueue()](auto action)
        {
            dispatcher.TryEnqueue([weak, action]
            {
                if (auto self = weak.get())
                {
                    action(*self);
                }
            });
        };
        ::AiUpscaler::EngineCallbacks callbacks;
        callbacks.Probe = [onUi](::AiUpscaler::JobProbe const& p) { onUi([p](MainWindow& w) { w.OnProbe(p); }); };
        callbacks.Preview = [onUi](::AiUpscaler::JobPreview const& p) { onUi([p](MainWindow& w) { w.OnPreview(p); }); };
        callbacks.Log = [onUi](std::wstring const& line) { onUi([line](MainWindow& w) { w.AppendLog(line); }); };
        callbacks.Exited = [onUi](::AiUpscaler::JobOutcome outcome, DWORD code) { onUi([outcome, code](MainWindow& w) { w.OnPreviewJobExited(outcome, code); }); };

        m_previewJob = std::make_unique<::AiUpscaler::EngineJob>(tools->Engine, std::move(args), std::move(callbacks));
        m_previewJobIsAi = aiFrame;
        try
        {
            m_previewJob->Start();
        }
        catch (...)
        {
            m_previewJob.reset();
            ShowResult(InfoBarSeverity::Error, T(L"Err_EngineStart"), std::wstring(to_message()), false);
            return;
        }
        (aiFrame ? OutputBusy() : SourceBusy()).IsActive(true);
    }

    void MainWindow::RequestSourceFrame(double seconds)
    {
        if (m_inputPath.empty() || (m_job && m_job->IsRunning()))
        {
            return;
        }
        auto tools = FindTools();
        if (!tools)
        {
            return;
        }
        StartPreviewJob({ m_inputPath, L"--frame-at", FormatSeconds(seconds), L"--source-only", L"--preview-dir", m_previewDir.wstring(),
                          L"--hwdec", L"none", L"--ffmpeg", tools->Ffmpeg.wstring(), L"--ffprobe", tools->Ffprobe.wstring() },
                        false);
    }

    void MainWindow::ScheduleAiPreview()
    {
        if (!m_aiPreviewTimer)
        {
            return; // still constructing (saved settings being applied)
        }
        m_aiPreviewTimer.Stop();
        if (m_inputPath.empty() || m_duration <= 0 || (m_job && m_job->IsRunning()))
        {
            return;
        }
        if (!unbox_value_or<bool>(AutoPreviewCheck().IsChecked(), false))
        {
            AutoPreviewStatus().Text(T(L"Auto_Off"));
            return;
        }
        AutoPreviewStatus().Text(T(L"Auto_Countdown"));
        m_aiPreviewTimer.Start();
    }

    void MainWindow::OnAutoPreviewChanged(IInspectable const&, RoutedEventArgs const&)
    {
        CollectSettings().Save();
        bool enabled = unbox_value_or<bool>(AutoPreviewCheck().IsChecked(), false);
        if (enabled && !OutputImage().Source())
        {
            RequestAiPreview(); // switched on with nothing shown: preview right away
            return;
        }
        ScheduleAiPreview();
        if (enabled)
        {
            m_aiPreviewTimer.Stop();
            AutoPreviewStatus().Text(L"");
        }
    }

    void MainWindow::OnPreviewAccelerator(Input::KeyboardAccelerator const&, Input::KeyboardAcceleratorInvokedEventArgs const& e)
    {
        e.Handled(true);
        RequestAiPreview();
    }

    void MainWindow::RequestAiPreview()
    {
        if (m_aiPreviewTimer)
        {
            m_aiPreviewTimer.Stop();
        }
        if (m_inputPath.empty() || m_duration <= 0 || (m_job && m_job->IsRunning()))
        {
            return;
        }
        auto settings = CollectSettings();
        if (!settings.ModelPath.empty() && !fs::exists(settings.ModelPath))
        {
            ShowResult(InfoBarSeverity::Warning, T(L"Err_ModelMissing_Title"), T(L"Err_ModelMissing"), false);
            return;
        }
        auto tools = FindTools();
        if (!tools)
        {
            return;
        }
        m_scrubTimer.Stop();

        std::vector<std::wstring> args{ m_inputPath, L"--frame-at", FormatSeconds(TimelineSlider().Value()), L"--preview-dir", m_previewDir.wstring(),
                                        L"--device", settings.Device, L"--tile", std::to_wstring(settings.TileSize), L"--hwdec", L"none",
                                        L"--ffmpeg", tools->Ffmpeg.wstring(), L"--ffprobe", tools->Ffprobe.wstring() };
        if (!settings.ModelPath.empty())
        {
            args.insert(args.end(), { L"--model", settings.ModelPath });
        }
        OutputPlaceholder().Visibility(OutputImage().Source() ? Visibility::Collapsed : Visibility::Visible);
        OutputPlaceholderText().Text(T(L"Placeholder_Upscaling"));
        AutoPreviewStatus().Text(F(L"Auto_Working", { FormatDuration(TimelineSlider().Value()) }));
        StartPreviewJob(std::move(args), true);
    }

    void MainWindow::OnProbe(::AiUpscaler::JobProbe const& p)
    {
        m_duration = p.Duration;
        m_sourceWidth = p.Width;
        m_sourceHeight = p.Height;
        m_sar = p.Sar > 0 ? p.Sar : 1.0;
        auto info = F(L"Source_Info", { std::to_wstring(p.Width), std::to_wstring(p.Height), AspectName(p.Width * m_sar / p.Height),
                                        Num(p.Fps, p.Fps == std::floor(p.Fps) ? 0 : 2), FormatDuration(p.Duration) });
        SourceInfoText().Text(p.Audio ? F(L"Source_WithAudio", { info }) : info);
        DurationText().Text(FormatDuration(p.Duration));
        FitImages();
        UpdateOutputSummary();
        StartThumbnails();

        auto lastFrame = p.Fps > 0 ? p.Duration - 1.0 / p.Fps : p.Duration;
        TimelineSlider().Maximum(std::max(lastFrame, 0.1));
        TimelineSlider().StepFrequency(p.Fps > 0 ? 1.0 / p.Fps : 0.04);
        TimelineSlider().IsEnabled(true);

        // Start a little way in: the first frames are often black or a title card.
        auto start = std::min(p.Duration * 0.1, 60.0);
        SetTimelinePosition(start);
        RequestSourceFrame(start);
        ScheduleAiPreview();
    }

    void MainWindow::OnPreview(::AiUpscaler::JobPreview const& p)
    {
        auto generation = ++m_imageGeneration;
        if (!p.SourcePath.empty())
        {
            LoadImage(SourceImage(), p.SourcePath, generation, true);
        }
        if (!p.OutputPath.empty())
        {
            m_outputPixelWidth = p.OutputWidth;
            m_outputPixelHeight = p.OutputHeight;
            if (p.SourceHeight > 0)
            {
                m_modelScale = p.OutputHeight / p.SourceHeight;
            }
            LoadImage(OutputImage(), p.OutputPath, generation, false);
            // The header shows what the file will contain, not just the raw AI frame.
            OutputInfoText().Text(F(L"OutputInfo", { std::to_wstring(m_modelScale), DescribeFinalSize(CollectSettings(), false), FormatDuration(p.Time) }));
            UpdateOutputSummary();
            OutputPlaceholder().Visibility(Visibility::Collapsed);
            if (!(m_job && m_job->IsRunning()))
            {
                AutoPreviewStatus().Text(L"");
            }
        }
        else
        {
            // A new source frame makes any shown AI result stale.
            OutputImage().Source(nullptr);
            OutputInfoText().Text(L"");
            OutputPlaceholder().Visibility(Visibility::Visible);
            OutputPlaceholderText().Text(unbox_value_or<bool>(AutoPreviewCheck().IsChecked(), false)
                                             ? F(L"Placeholder_Coming", { FormatDuration(p.Time) })
                                             : F(L"Placeholder_Off", { FormatDuration(p.Time) }));
        }
        if (p.Frame >= 0)
        {
            SetTimelinePosition(p.Time); // live preview during a run
        }
    }

    void MainWindow::OnPreviewJobExited(::AiUpscaler::JobOutcome outcome, DWORD)
    {
        bool wasAi = m_previewJobIsAi;
        m_previewJob.reset();
        SourceBusy().IsActive(false);
        OutputBusy().IsActive(false);

        if (outcome == ::AiUpscaler::JobOutcome::Failed)
        {
            if (wasAi)
            {
                OutputPlaceholder().Visibility(Visibility::Visible);
                OutputPlaceholderText().Text(T(L"Placeholder_PreviewFailed"));
                AutoPreviewStatus().Text(L"");
            }
            else if (m_duration <= 0)
            {
                SourceInfoText().Text(T(L"Source_Unreadable"));
                ShowResult(InfoBarSeverity::Error, T(L"Err_ReadVideo_Title"), T(L"Err_SeeLog"), false);
            }
        }

        if (m_pendingPreviewArgs)
        {
            auto args = std::move(*m_pendingPreviewArgs);
            m_pendingPreviewArgs.reset();
            StartPreviewJob(std::move(args), m_pendingPreviewIsAi);
        }
    }

    winrt::fire_and_forget MainWindow::LoadImage(Image target, std::wstring path, uint64_t generation, bool isSource)
    {
        auto lifetime = get_strong();
        auto dispatcher = DispatcherQueue();
        std::wstring error;
        try
        {
            auto file = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path);
            auto stream = co_await file.OpenReadAsync();
            co_await wil::resume_foreground(dispatcher);
            Media::Imaging::BitmapImage bitmap;
            co_await bitmap.SetSourceAsync(stream);
            co_await wil::resume_foreground(dispatcher);

            // Images can finish loading out of order; never replace a newer one.
            auto& shown = isSource ? m_shownSourceGeneration : m_shownOutputGeneration;
            if (generation < shown)
            {
                co_return;
            }
            shown = generation;
            target.Source(bitmap);
            FitImages();
        }
        catch (hresult_error const& e)
        {
            error = e.message();
        }
        if (!error.empty())
        {
            co_await wil::resume_foreground(dispatcher);
            AppendLog(F(L"Log_ImageFailed", { path, error }));
        }
    }

    // ---- Film strip ------------------------------------------------------------------------------

    void MainWindow::StartThumbnails()
    {
        if (m_duration <= 0 || m_sourceHeight <= 0)
        {
            return;
        }
        auto tools = FindTools();
        if (!tools)
        {
            return;
        }

        // As many frames as fit the strip at the picture's shape.
        constexpr double FrameHeight = 64;
        double frameWidth = FrameHeight * m_sourceWidth * m_sar / m_sourceHeight + 3;
        double stripWidth = FilmStrip().ActualWidth() > 0 ? FilmStrip().ActualWidth() : RootGrid().ActualWidth() - 32;
        int count = std::clamp(static_cast<int>(stripWidth / frameWidth), 4, 40);
        BuildFilmFrames(count);
        FilmStrip().Visibility(Visibility::Visible);

        auto onUi = [weak = get_weak(), dispatcher = DispatcherQueue()](auto action)
        {
            dispatcher.TryEnqueue([weak, action]
            {
                if (auto self = weak.get())
                {
                    action(*self);
                }
            });
        };
        ::AiUpscaler::EngineCallbacks callbacks;
        callbacks.Other = [onUi](std::wstring const& event, Windows::Data::Json::JsonObject const& obj)
        {
            if (event == L"thumbnail")
            {
                onUi([obj](MainWindow& w) { w.OnThumbnail(obj); });
            }
        };
        callbacks.Log = [onUi](std::wstring const& line) { onUi([line](MainWindow& w) { w.AppendLog(line); }); };
        callbacks.Exited = [onUi](::AiUpscaler::JobOutcome, DWORD) { onUi([](MainWindow& w) { w.m_thumbJob.reset(); }); };

        // Own folder: thumbnails are not rotated out like live preview frames.
        auto dir = m_previewDir / L"thumbs";
        std::vector<std::wstring> args{ m_inputPath, L"--thumbnails", std::to_wstring(count), L"--thumb-height", L"128",
                                        L"--preview-dir", dir.wstring(), L"--ffmpeg", tools->Ffmpeg.wstring(), L"--ffprobe", tools->Ffprobe.wstring() };
        m_thumbJob = std::make_unique<::AiUpscaler::EngineJob>(tools->Engine, std::move(args), std::move(callbacks));
        try
        {
            m_thumbJob->Start();
        }
        catch (...)
        {
            m_thumbJob.reset();
            AppendLog(F(L"Log_ThumbsFailed", { std::wstring(to_message()) }));
        }
    }

    void MainWindow::BuildFilmFrames(int count)
    {
        auto frames = FilmFrames();
        frames.Children().Clear();
        frames.ColumnDefinitions().Clear();
        m_thumbTimes.clear();

        for (int i = 0; i < count; ++i)
        {
            ColumnDefinition column;
            column.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            frames.ColumnDefinitions().Append(column);

            double time = m_duration * (i + 0.5) / count;
            m_thumbTimes.push_back(time);

            Border cell;
            cell.Background(Media::SolidColorBrush(Microsoft::UI::ColorHelper::FromArgb(255, 34, 34, 34)));
            cell.CornerRadius(CornerRadiusHelper::FromUniformRadius(2));
            Image image;
            image.Stretch(Media::Stretch::UniformToFill);
            cell.Child(image);
            Grid::SetColumn(cell, i);
            ToolTipService::SetToolTip(cell, box_value(F(L"Film_GoTo", { FormatDuration(time) })));
            cell.Tapped([this, time](auto&&, auto&&)
            {
                if (m_job && m_job->IsRunning())
                {
                    return;
                }
                SetTimelinePosition(time);
                RequestSourceFrame(time);
                ScheduleAiPreview();
            });
            frames.Children().Append(cell);
        }
        LayoutSprockets();
        UpdatePlayhead();
    }

    void MainWindow::OnThumbnail(Windows::Data::Json::JsonObject const& thumbnail)
    {
        auto index = static_cast<uint32_t>(thumbnail.GetNamedNumber(L"index", -1));
        auto cells = FilmFrames().Children();
        if (index >= cells.Size())
        {
            return;
        }
        if (auto cell = cells.GetAt(index).try_as<Border>())
        {
            LoadThumbnail(cell.Child().as<Image>(), std::wstring(thumbnail.GetNamedString(L"path", L"")));
        }
    }

    winrt::fire_and_forget MainWindow::LoadThumbnail(Image target, std::wstring path)
    {
        auto lifetime = get_strong();
        auto dispatcher = DispatcherQueue();
        try
        {
            auto file = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path);
            auto stream = co_await file.OpenReadAsync();
            co_await wil::resume_foreground(dispatcher);
            Media::Imaging::BitmapImage bitmap;
            bitmap.DecodePixelHeight(128);
            co_await bitmap.SetSourceAsync(stream);
            co_await wil::resume_foreground(dispatcher);
            target.Source(bitmap);
        }
        catch (hresult_error const&)
        {
            // A thumbnail that can't be read just leaves a dark cell.
        }
    }

    void MainWindow::LayoutSprockets()
    {
        // Sprocket holes along both edges, like 35 mm film stock.
        constexpr double HoleWidth = 10;
        constexpr double Gap = 8;
        double width = FilmStrip().ActualWidth();
        int holes = width > 12 ? static_cast<int>((width - 12 + Gap) / (HoleWidth + Gap)) : 0;
        for (auto row : { SprocketsTop(), SprocketsBottom() })
        {
            if (static_cast<int>(row.Children().Size()) == holes)
            {
                continue;
            }
            row.Children().Clear();
            for (int i = 0; i < holes; ++i)
            {
                Shapes::Rectangle hole;
                hole.Width(HoleWidth);
                hole.Height(6);
                hole.RadiusX(1.5);
                hole.RadiusY(1.5);
                hole.Fill(Media::SolidColorBrush(Microsoft::UI::ColorHelper::FromArgb(255, 78, 78, 78)));
                row.Children().Append(hole);
            }
        }
    }

    void MainWindow::UpdatePlayhead()
    {
        if (!FilmStrip() || m_duration <= 0)
        {
            return;
        }
        double width = FilmStrip().ActualWidth();
        double position = std::clamp(TimelineSlider().Value() / m_duration, 0.0, 1.0);
        Canvas::SetLeft(FilmPlayhead(), 3 + position * std::max(0.0, width - 6) - FilmPlayhead().Width() / 2);
    }

    void MainWindow::OnFilmStripSizeChanged(IInspectable const&, SizeChangedEventArgs const&)
    {
        FilmPlayhead().Height(FilmStrip().ActualHeight());
        LayoutSprockets();
        UpdatePlayhead();
    }

    // ---- Hardware capabilities and output description -----------------------------------------------

    void MainWindow::StartCapabilities()
    {
        auto engine = ::AiUpscaler::ToolLocator::FindEngine();
        auto ffmpeg = ::AiUpscaler::ToolLocator::FindFfmpegTool(L"ffmpeg.exe");
        if (!engine || !ffmpeg)
        {
            return; // reported when the user opens a video
        }
        HardwareText().Text(T(L"Hw_Checking"));

        auto onUi = [weak = get_weak(), dispatcher = DispatcherQueue()](auto action)
        {
            dispatcher.TryEnqueue([weak, action]
            {
                if (auto self = weak.get())
                {
                    action(*self);
                }
            });
        };
        ::AiUpscaler::EngineCallbacks callbacks;
        callbacks.Other = [onUi](std::wstring const& event, Windows::Data::Json::JsonObject const& obj)
        {
            if (event == L"capabilities")
            {
                onUi([obj](MainWindow& w) { w.OnCapabilities(obj); });
            }
        };
        callbacks.Exited = [onUi](::AiUpscaler::JobOutcome, DWORD) { onUi([](MainWindow& w) { w.m_capsJob.reset(); }); };
        m_capsJob = std::make_unique<::AiUpscaler::EngineJob>(*engine, std::vector<std::wstring>{ L"--capabilities", L"--ffmpeg", ffmpeg->wstring() },
                                                               std::move(callbacks));
        try
        {
            m_capsJob->Start();
        }
        catch (...)
        {
            m_capsJob.reset();
        }
    }

    void MainWindow::SetItemAvailable(ComboBox const& combo, std::wstring const& tag, bool available, std::wstring const& label,
                                      std::wstring const& reason)
    {
        auto items = combo.Items();
        for (uint32_t i = 0; i < items.Size(); ++i)
        {
            auto item = items.GetAt(i).as<ComboBoxItem>();
            if (unbox_value<hstring>(item.Tag()) != tag)
            {
                continue;
            }
            item.Content(box_value(available || reason.empty() ? label : F(L"NotAvailable", { label })));
            item.IsEnabled(available);
            ToolTipService::SetToolTip(item, available || reason.empty() ? nullptr : box_value(reason));

            // A closed ComboBox keeps showing the selected item's old text; re-select to refresh it.
            if (available && combo.SelectedIndex() == static_cast<int32_t>(i))
            {
                m_refreshingItems = true;
                combo.SelectedIndex(-1);
                combo.SelectedIndex(static_cast<int32_t>(i));
                m_refreshingItems = false;
            }

            // Never leave an option selected that this PC can't do.
            if (!available && combo.SelectedIndex() == static_cast<int32_t>(i))
            {
                for (uint32_t j = 0; j < items.Size(); ++j)
                {
                    if (items.GetAt(j).as<ComboBoxItem>().IsEnabled())
                    {
                        combo.SelectedIndex(static_cast<int32_t>(j));
                        break;
                    }
                }
            }
            return;
        }
    }

    void MainWindow::OnCapabilities(Windows::Data::Json::JsonObject const& capabilities)
    {
        auto devices = capabilities.GetNamedObject(L"devices", nullptr);
        auto encoders = capabilities.GetNamedObject(L"encoders", nullptr);
        if (!devices || !encoders)
        {
            return;
        }
        // Options this PC can't run may be swapped for ones it can; that's not a change the user made.
        bool wasSaved = !HasUnsavedChanges();

        bool directml = devices.GetNamedBoolean(L"directml", false);
        bool cuda = devices.GetNamedBoolean(L"cuda", false);
        SetItemAvailable(DeviceCombo(), L"auto", true, T(cuda ? L"Device_AutoCuda" : directml ? L"Device_AutoDml" : L"Device_AutoCpu"), L"");
        SetItemAvailable(DeviceCombo(), L"directml", directml, T(L"Device_DirectML"), T(L"Device_NoDml"));
        // Say exactly why CUDA is missing: an engine built without it, or a CUDA/cuDNN runtime problem.
        auto errors = devices.GetNamedObject(L"errors", nullptr);
        std::wstring cudaError = errors ? std::wstring(errors.GetNamedString(L"cuda", L"")) : L"";
        std::wstring cudaReason = T(L"Device_NoCuda");
        if (cudaError == L"not_built")
        {
            cudaReason = T(L"Device_CudaNotBuilt");
        }
        else if (!cudaError.empty())
        {
            cudaReason = F(L"Device_CudaRuntime", { cudaError });
            AppendLog(L"CUDA unavailable: " + cudaError);
        }
        SetItemAvailable(DeviceCombo(), L"cuda", cuda, T(L"Device_Cuda"), cudaReason);

        struct Family { wchar_t const* Tag; wchar_t const* Name; };
        std::wstring gpuEncoders;
        std::wstring gpuVendor;
        for (auto const& family : { Family{ L"auto", L"H.264" }, Family{ L"auto-hevc", L"HEVC" }, Family{ L"auto-av1", L"AV1" } })
        {
            auto value = encoders.TryLookup(family.Tag);
            std::wstring encoder = value && value.ValueType() == Windows::Data::Json::JsonValueType::String ? std::wstring(value.GetString()) : L"";
            std::wstring vendor = encoder.ends_with(L"_amf") ? L"AMD" : encoder.ends_with(L"_nvenc") ? L"NVIDIA" : encoder.ends_with(L"_qsv") ? L"Intel" : L"";
            bool ok = !encoder.empty();
            SetItemAvailable(CodecCombo(), family.Tag, ok, ok ? F(L"Codec_Gpu", { family.Name, vendor }) : F(L"Codec_GpuPlain", { family.Name }),
                             F(L"Codec_NoGpu", { family.Name }));
            if (ok)
            {
                gpuEncoders += (gpuEncoders.empty() ? L"" : L", ") + std::wstring(family.Name);
                gpuVendor = vendor;
            }
        }
        struct Software { wchar_t const* Tag; wchar_t const* Label; };
        for (auto const& sw : { Software{ L"libx264", L"H.264 – CPU (x264)" }, Software{ L"libx265", L"HEVC – CPU (x265)" },
                                Software{ L"libsvtav1", L"AV1 – CPU (SVT-AV1)" } })
        {
            SetItemAvailable(CodecCombo(), sw.Tag, encoders.GetNamedBoolean(sw.Tag, false), sw.Label, T(L"Codec_NotInFfmpeg"));
        }

        HardwareText().Text(F(L"Hw_Summary", { T(cuda ? L"Hw_AiCuda" : directml ? L"Hw_AiDml" : L"Hw_AiCpu"),
                                               gpuEncoders.empty() ? T(L"Hw_NoGpuEncode") : F(L"Hw_GpuEncodes", { gpuVendor, gpuEncoders }) }));
        UpdateOutputSummary();
        if (wasSaved)
        {
            m_savedConfig = CollectSettings();
        }
        UpdateConfigTitle();
    }

    std::wstring MainWindow::DescribeFinalSize(::AiUpscaler::Settings const& settings, bool withNote)
    {
        if (m_sourceHeight <= 0)
        {
            return L"";
        }
        double aspect = m_sourceWidth * m_sar / m_sourceHeight;
        auto size = [](int w, int h) { return std::to_wstring(w) + L"×" + std::to_wstring(h); };

        if (settings.TargetHeight > 0)
        {
            // Always the standard frame (e.g. 2160p = 3840×2160); only how the picture fills it varies.
            auto frame = size(FrameWidthFor(settings.TargetHeight), settings.TargetHeight);
            if (!withNote)
            {
                return frame;
            }
            if (std::abs(aspect - 16.0 / 9.0) < 0.02)
            {
                return F(L"Size_Fills", { frame });
            }
            if (settings.Fit == L"crop")
            {
                return F(L"Size_Crop", { frame, AspectName(aspect) });
            }
            if (settings.Fit == L"stretch")
            {
                return F(L"Size_Stretch", { frame, AspectName(aspect) });
            }
            return F(aspect < 16.0 / 9.0 ? L"Size_PadSides" : L"Size_PadTopBottom", { frame, AspectName(aspect) });
        }
        if (m_modelScale > 0)
        {
            auto native = size(Even(m_sourceWidth * m_sar * m_modelScale), Even(m_sourceHeight * static_cast<double>(m_modelScale)));
            return withNote ? F(L"Size_Model", { native, std::to_wstring(m_modelScale) }) : native;
        }
        return T(withNote ? L"Size_ModelUnknown" : L"Size_ModelShort");
    }

    void MainWindow::UpdateOutputSummary()
    {
        if (!FinalSizeText() || !OutputSummaryText() || !CodecCombo())
        {
            return;
        }
        auto settings = CollectSettings();
        if (m_sourceHeight <= 0)
        {
            FinalSizeText().Text(T(L"Size_OpenVideo"));
            OutputSummaryText().Text(T(L"Summary_Begin"));
            return;
        }
        FinalSizeText().Text(DescribeFinalSize(settings, true));

        std::wstring codec;
        if (auto item = CodecCombo().SelectedItem().try_as<ComboBoxItem>())
        {
            codec = unbox_value_or<hstring>(item.Content(), L"").c_str();
        }
        auto summary = F(L"Summary", { DescribeFinalSize(settings, false), codec, std::to_wstring(settings.Quality) });
        OutputSummaryText().Text(settings.ModelPath.empty() ? F(L"Summary_NoModel", { summary }) : summary);
    }

    // ---- Timeline -------------------------------------------------------------------------------

    void MainWindow::SetTimelinePosition(double seconds)
    {
        m_updatingTimeline = true;
        TimelineSlider().Value(seconds);
        m_updatingTimeline = false;
        TimeText().Text(FormatDuration(seconds));
        UpdatePlayhead();
    }

    void MainWindow::OnTimelineChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (!TimeText())
        {
            return; // during InitializeComponent
        }
        TimeText().Text(FormatDuration(e.NewValue()));
        UpdatePlayhead();
        if (m_updatingTimeline || m_inputPath.empty())
        {
            return;
        }
        m_scrubTimer.Stop();
        m_scrubTimer.Start();
        ScheduleAiPreview(); // restarts the 3 s wait on every move
    }

    // ---- Synced zoom and pan ---------------------------------------------------------------------

    void MainWindow::FitImages()
    {
        // The picture is the largest rectangle of the video's display shape that fits, centred
        // in its frame. Sizing it explicitly (instead of relying on Stretch=Uniform) keeps it
        // centred at any window size and shows anamorphic DVDs at their true shape.
        double aspect = 0;
        if (m_sourceHeight > 0)
        {
            aspect = m_sourceWidth * m_sar / m_sourceHeight;
        }
        else if (auto bitmap = SourceImage().Source().try_as<Media::Imaging::BitmapImage>(); bitmap && bitmap.PixelHeight() > 0)
        {
            aspect = static_cast<double>(bitmap.PixelWidth()) / bitmap.PixelHeight();
        }

        // Both panes use the same frame size (the smaller of the two viewports; the Upscaled
        // pane is shorter because of the preview tickbox). Identical frames mean identical zoom
        // and scroll offsets show exactly the same pixels side by side; each frame is centred
        // in its pane by the ScrollViewer's content alignment.
        double width = std::min(SourceScroller().ActualWidth(), OutputScroller().ActualWidth());
        double height = std::min(SourceScroller().ActualHeight(), OutputScroller().ActualHeight());
        if (width <= 0 || height <= 0)
        {
            return;
        }
        for (auto const& [frame, picture] : { std::pair{ SourceFrame(), SourceImage() }, std::pair{ OutputFrame(), OutputImage() } })
        {
            frame.Width(width);
            frame.Height(height);
            if (aspect > 0)
            {
                double pictureWidth = std::min(width, height * aspect);
                picture.Width(pictureWidth);
                picture.Height(pictureWidth / aspect);
            }
        }
    }

    void MainWindow::OnScrollerSizeChanged(IInspectable const&, SizeChangedEventArgs const&)
    {
        FitImages();
    }

    void MainWindow::SyncView(ScrollViewer const& from, ScrollViewer const& to, ScrollViewerViewChangedEventArgs const& e)
    {
        if (m_programmaticScroll == from)
        {
            // This is the echo of a move we made in code; don't mirror it back.
            if (!e.IsIntermediate())
            {
                m_programmaticScroll = nullptr;
            }
            return;
        }
        bool same = std::abs(to.ZoomFactor() - from.ZoomFactor()) < 0.001f && std::abs(to.HorizontalOffset() - from.HorizontalOffset()) < 0.5 &&
                    std::abs(to.VerticalOffset() - from.VerticalOffset()) < 0.5;
        if (same)
        {
            return;
        }
        m_programmaticScroll = to;
        to.ChangeView(from.HorizontalOffset(), from.VerticalOffset(), from.ZoomFactor(), true);
    }

    void MainWindow::OnSourceViewChanged(IInspectable const&, ScrollViewerViewChangedEventArgs const& e)
    {
        SyncView(SourceScroller(), OutputScroller(), e);
        if (!e.IsIntermediate())
        {
            ShowZoomState(SourceScroller().ZoomFactor() > 1.01f); // keeps the toggle honest after wheel/pinch zoom
        }
    }

    void MainWindow::OnOutputViewChanged(IInspectable const&, ScrollViewerViewChangedEventArgs const& e)
    {
        SyncView(OutputScroller(), SourceScroller(), e);
        if (!e.IsIntermediate())
        {
            ShowZoomState(OutputScroller().ZoomFactor() > 1.01f);
        }
    }

    void MainWindow::OnZoomToggle(IInspectable const&, RoutedEventArgs const&)
    {
        SetZoom(unbox_value_or<bool>(ZoomToggle().IsChecked(), false));
    }

    void MainWindow::ShowZoomState(bool actualPixels)
    {
        ZoomToggle().IsChecked(actualPixels);
        ZoomToggleText().Text(actualPixels ? std::wstring(L"1:1") : T(L"Zoom_Fit"));
        ZoomToggleIcon().Glyph(actualPixels ? L"" : L""); // Zoom / FitPage
    }

    void MainWindow::SetZoom(bool actualPixels)
    {
        ShowZoomState(actualPixels);
        m_programmaticScroll = OutputScroller();
        if (!actualPixels)
        {
            SourceScroller().ChangeView(0.0, 0.0, 1.0f);
            OutputScroller().ChangeView(0.0, 0.0, 1.0f);
            return;
        }

        auto scroller = OutputScroller();
        double boxWidth = scroller.ActualWidth();
        double boxHeight = scroller.ActualHeight();
        double pictureHeight = OutputImage().Height();
        // 1:1 for the upscaled frame; before any AI preview exists, 1:1 for the source.
        int pixelHeight = m_outputPixelHeight > 0 && OutputImage().Source() ? m_outputPixelHeight : m_sourceHeight;
        if (pixelHeight <= 0 || boxWidth <= 0 || boxHeight <= 0 || std::isnan(pictureHeight) || pictureHeight <= 0)
        {
            ShowZoomState(false);
            return;
        }
        // Height the picture occupies at zoom 1, in physical pixels (height, because anamorphic
        // frames are widened for display).
        double shownHeight = pictureHeight * Content().XamlRoot().RasterizationScale();
        auto zoom = static_cast<float>(std::clamp(pixelHeight / shownHeight, 1.0, 16.0));
        // Keep the centre of the frame in view.
        double h = (boxWidth * zoom - boxWidth) / 2;
        double v = (boxHeight * zoom - boxHeight) / 2;
        SourceScroller().ChangeView(h, v, zoom);
        OutputScroller().ChangeView(h, v, zoom);
    }

    // ---- Bottom panel ----------------------------------------------------------------------------

    void MainWindow::OnBottomTabChanged(SelectorBar const& sender, SelectorBarSelectionChangedEventArgs const&)
    {
        ShowBottomTab(sender.SelectedItem());
    }

    void MainWindow::ShowBottomTab(SelectorBarItem const& tab)
    {
        if (!ProgressPanel() || !UpscaleSettingsPanel() || !BatchPanel())
        {
            return; // during InitializeComponent
        }
        if (BottomTabs().SelectedItem() != tab)
        {
            BottomTabs().SelectedItem(tab);
        }
        ProgressPanel().Visibility(tab == ProgressTab() ? Visibility::Visible : Visibility::Collapsed);
        UpscaleSettingsPanel().Visibility(tab == UpscaleSettingsTab() ? Visibility::Visible : Visibility::Collapsed);
        BatchPanel().Visibility(tab == BatchTab() ? Visibility::Visible : Visibility::Collapsed);
    }

    // ---- App settings page -----------------------------------------------------------------------

    void MainWindow::OnOpenAppSettings(IInspectable const&, RoutedEventArgs const&)
    {
        LoadAppSettingsPage();
        AppSettingsView().Visibility(Visibility::Visible);
        if (!SettingsNavList().SelectedItem())
        {
            SettingsNavList().SelectedItem(AppearanceNavItem());
        }
        SettingsBackButton().Focus(FocusState::Programmatic);
    }

    void MainWindow::OnSettingsBack(IInspectable const&, RoutedEventArgs const&)
    {
        AppSettingsView().Visibility(Visibility::Collapsed);
        UpdateOutputSummary();
    }

    void MainWindow::OnSettingsEscape(Input::KeyboardAccelerator const&, Input::KeyboardAcceleratorInvokedEventArgs const& e)
    {
        e.Handled(true);
        OnSettingsBack(nullptr, nullptr);
    }

    void MainWindow::OnSettingsNavChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (auto item = SettingsNavList().SelectedItem().try_as<ListViewItem>())
        {
            ShowSettingsSection(std::wstring(unbox_value_or<hstring>(item.Tag(), L"")));
        }
    }

    void MainWindow::ShowSettingsSection(std::wstring const& tag)
    {
        AppearanceSection().Visibility(tag == L"appearance" ? Visibility::Visible : Visibility::Collapsed);
        OutputSection().Visibility(tag == L"output" ? Visibility::Visible : Visibility::Collapsed);
        ModelsSection().Visibility(tag == L"models" ? Visibility::Visible : Visibility::Collapsed);
        SettingsSectionTitle().Text(T(tag == L"output" ? L"Section_Output" : tag == L"models" ? L"Section_Models" : L"Section_Appearance"));
        if (tag == L"models")
        {
            PopulateModelsList();
        }
    }

    void MainWindow::LoadAppSettingsPage()
    {
        m_loadingAppSettings = true;
        SelectByTag(ThemeCombo(), m_appSettings.Theme);

        auto languages = LanguageCombo();
        languages.Items().Clear();
        languages.Items().Append(MakeItem(T(L"Lang_System"), L""));
        for (auto const& language : ::AiUpscaler::Strings::Available())
        {
            languages.Items().Append(MakeItem(language.NativeName, language.Tag));
        }
        SelectByTag(languages, m_appSettings.Language);
        LanguageRestartBar().IsOpen(m_appSettings.Language != m_startupLanguage);

        bool hasFolder = !m_appSettings.OutputFolder.empty();
        OutputNextToSourceRadio().IsChecked(!hasFolder);
        OutputFolderRadio().IsChecked(hasFolder);
        OutputFolderText().Text(hasFolder ? m_appSettings.OutputFolder : T(L"OutputFolder_None"));
        m_loadingAppSettings = false;
    }

    void MainWindow::SaveAppSettings()
    {
        auto settings = CollectSettings();
        settings.Theme = m_appSettings.Theme;
        settings.Language = m_appSettings.Language;
        settings.OutputFolder = m_appSettings.OutputFolder;
        settings.Save();
    }

    void MainWindow::ApplyTheme(std::wstring const& theme)
    {
        using Microsoft::UI::Windowing::TitleBarTheme;
        auto element = theme == L"light" ? ElementTheme::Light : theme == L"dark" ? ElementTheme::Dark : ElementTheme::Default;
        RootGrid().RequestedTheme(element);
        // Caption buttons (minimise/maximise/close) follow the same theme.
        AppWindow().TitleBar().PreferredTheme(theme == L"light" ? TitleBarTheme::Light : theme == L"dark" ? TitleBarTheme::Dark : TitleBarTheme::UseDefaultAppMode);
        UpdateCaptionButtons();
    }

    void MainWindow::UpdateCaptionButtons()
    {
        using winrt::Windows::UI::ViewManagement::UIColorType;
        auto titleBar = AppWindow().TitleBar();
        if (RootGrid().ActualTheme() == ElementTheme::Light)
        {
            // AccentDark1 is the accent shade Windows uses for accent text on light surfaces,
            // so the glyphs stay readable on the light Mica title bar.
            auto accent = m_uiSettings.GetColorValue(UIColorType::AccentDark1);
            titleBar.ButtonForegroundColor(accent);
            titleBar.ButtonHoverForegroundColor(accent);
            titleBar.ButtonPressedForegroundColor(accent);
            auto inactive = accent;
            inactive.A = 0x99; // dimmed while the window is in the background, like the defaults
            titleBar.ButtonInactiveForegroundColor(inactive);
        }
        else
        {
            titleBar.ButtonForegroundColor(nullptr);
            titleBar.ButtonHoverForegroundColor(nullptr);
            titleBar.ButtonPressedForegroundColor(nullptr);
            titleBar.ButtonInactiveForegroundColor(nullptr);
        }
    }

    void MainWindow::OnThemeChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_loadingAppSettings || !ThemeCombo())
        {
            return;
        }
        m_appSettings.Theme = SelectedTag(ThemeCombo());
        ApplyTheme(m_appSettings.Theme);
        SaveAppSettings();
    }

    void MainWindow::OnLanguageChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_loadingAppSettings || !LanguageCombo())
        {
            return;
        }
        m_appSettings.Language = SelectedTag(LanguageCombo());
        SaveAppSettings();
        // The new language takes effect at the next start.
        LanguageRestartBar().IsOpen(m_appSettings.Language != m_startupLanguage);
    }

    fire_and_forget MainWindow::OnRestart(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        if (m_job && m_job->IsRunning())
        {
            LanguageRestartBar().Message(T(L"Lang_RestartBusy"));
            co_return;
        }
        if (!co_await ConfirmUnsavedChangesAsync())
        {
            co_return;
        }
        wchar_t exe[MAX_PATH * 4]{};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        // Reopen the current video in the restarted app.
        std::wstring args = m_inputPath.empty() ? L"" : L"\"" + m_inputPath + L"\"";
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe, args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL)) > 32)
        {
            m_closeConfirmed = true;
            Close();
        }
    }

    void MainWindow::OnOutputLocationChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loadingAppSettings)
        {
            return;
        }
        if (unbox_value_or<bool>(OutputNextToSourceRadio().IsChecked(), false))
        {
            m_appSettings.OutputFolder.clear();
            OutputFolderText().Text(T(L"OutputFolder_None"));
            SaveAppSettings();
        }
        else if (m_appSettings.OutputFolder.empty())
        {
            OnChooseOutputFolder(nullptr, nullptr); // "This folder" needs a folder
        }
        // Follow the new default unless the user picked their own output file.
        if (!m_inputPath.empty() && std::wstring(OutputBox().Text()) == m_autoOutput)
        {
            m_autoOutput = DefaultOutputFor(m_inputPath);
            OutputBox().Text(m_autoOutput);
        }
    }

    fire_and_forget MainWindow::OnChooseOutputFolder(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        FolderPicker picker(AppWindow().Id());
        picker.SuggestedStartLocation(PickerLocationId::VideosLibrary);
        auto result = co_await picker.PickSingleFolderAsync();
        if (result)
        {
            m_appSettings.OutputFolder = result.Path();
            SaveAppSettings();
        }
        LoadAppSettingsPage(); // reflect the folder, or fall back to "next to source" if none was picked
        if (!m_inputPath.empty() && std::wstring(OutputBox().Text()) == m_autoOutput)
        {
            m_autoOutput = DefaultOutputFor(m_inputPath);
            OutputBox().Text(m_autoOutput);
        }
    }

    // ---- Model library ---------------------------------------------------------------------------

    void MainWindow::RefreshModels(std::wstring const& select)
    {
        m_refreshingModels = true;
        auto combo = ModelCombo();
        combo.Items().Clear();
        combo.Items().Append(MakeItem(T(L"Model_None"), L""));

        bool found = select.empty();
        for (auto const& model : ::AiUpscaler::ModelLibrary::List())
        {
            combo.Items().Append(MakeItem(model.Name, model.Path.wstring()));
            std::error_code ec;
            found = found || fs::equivalent(model.Path, select, ec);
        }
        // A model chosen before the library existed stays usable until it is imported.
        if (!found && fs::exists(select))
        {
            combo.Items().Append(MakeItem(F(L"Model_External", { fs::path(select).stem().wstring() }), select));
        }

        combo.SelectedIndex(0);
        for (uint32_t i = 0; i < combo.Items().Size(); ++i)
        {
            std::error_code ec;
            auto tag = std::wstring(unbox_value<hstring>(combo.Items().GetAt(i).as<ComboBoxItem>().Tag()));
            if (!select.empty() && !tag.empty() && fs::equivalent(tag, select, ec))
            {
                combo.SelectedIndex(static_cast<int32_t>(i));
                break;
            }
        }
        m_refreshingModels = false;
        PopulateModelsList();
    }

    void MainWindow::PopulateModelsList()
    {
        if (!ModelsList())
        {
            return;
        }
        ModelsFolderText().Text(F(L"Models_Folder", { ::AiUpscaler::ModelLibrary::Folder().wstring() }));
        auto list = ModelsList();
        list.Items().Clear();
        auto selected = SelectedTag(ModelCombo());
        auto models = ::AiUpscaler::ModelLibrary::List();
        for (auto const& model : models)
        {
            Grid row;
            row.ColumnSpacing(12);
            row.Padding(ThicknessHelper::FromLengths(0, 6, 0, 6));
            for (auto width : { GridLengthHelper::Auto(), GridLengthHelper::FromValueAndType(1, GridUnitType::Star), GridLengthHelper::Auto() })
            {
                ColumnDefinition column;
                column.Width(width);
                row.ColumnDefinitions().Append(column);
            }

            FontIcon icon;
            icon.Glyph(L"");
            icon.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(icon);

            StackPanel text;
            text.VerticalAlignment(VerticalAlignment::Center);
            TextBlock name;
            std::error_code ec;
            bool inUse = !selected.empty() && fs::equivalent(model.Path, selected, ec);
            name.Text(inUse ? F(L"Models_InUse", { model.Name }) : model.Name);
            name.Style(Application::Current().Resources().Lookup(box_value(L"BodyStrongTextBlockStyle")).as<Style>());
            TextBlock details;
            details.Text(F(L"Models_Details", { FileSize(model.SizeBytes), model.Path.filename().wstring() }));
            details.Style(Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle")).as<Style>());
            text.Children().Append(name);
            text.Children().Append(details);
            Grid::SetColumn(text, 1);
            row.Children().Append(text);

            Button remove;
            FontIcon removeIcon;
            removeIcon.Glyph(L"");
            removeIcon.FontSize(14);
            remove.Content(removeIcon);
            ToolTipService::SetToolTip(remove, box_value(T(L"Models_RemoveTip")));
            remove.IsEnabled(!(m_job && m_job->IsRunning()));
            Grid::SetColumn(remove, 2);
            auto path = model.Path;
            auto displayName = model.Name;
            remove.Click([this, path, displayName](auto&&, auto&&)
            {
                try
                {
                    ::AiUpscaler::ModelLibrary::Remove(path);
                    ShowModelsResult(InfoBarSeverity::Success, F(L"Models_Removed", { displayName }));
                }
                catch (std::exception const&)
                {
                    ShowModelsResult(InfoBarSeverity::Error, F(L"Models_RemoveFailed", { displayName }));
                }
                std::error_code ec;
                auto current = SelectedTag(ModelCombo());
                RefreshModels(fs::equivalent(path, current, ec) || !fs::exists(current) ? L"" : current);
                UpdateOutputSummary();
            });
            row.Children().Append(remove);
            list.Items().Append(row);
        }
        ModelsEmptyText().Visibility(models.empty() ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::ShowModelsResult(InfoBarSeverity severity, std::wstring const& message)
    {
        ModelsResultBar().Severity(severity);
        ModelsResultBar().Message(message);
        ModelsResultBar().IsOpen(true);
    }

    fire_and_forget MainWindow::OnImportModel(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        FileOpenPicker picker(AppWindow().Id());
        picker.FileTypeFilter().Append(L".onnx");
        auto result = co_await picker.PickSingleFileAsync();
        if (!result)
        {
            co_return;
        }
        fs::path source{ std::wstring(result.Path()) };
        auto name = source.stem().wstring();
        try
        {
            auto imported = ::AiUpscaler::ModelLibrary::Import(source);
            ShowModelsResult(InfoBarSeverity::Success, F(L"Models_Imported", { name }));
            // A first model, or the external one just imported, becomes the selection.
            auto current = SelectedTag(ModelCombo());
            std::error_code ec;
            bool useImported = current.empty() || fs::equivalent(current, source, ec) || !fs::exists(current);
            RefreshModels(useImported ? imported.wstring() : current);
            CollectSettings().Save();
            UpdateOutputSummary();
        }
        catch (fs::filesystem_error const& e)
        {
            // Before runtime_error: filesystem_error derives from it.
            ShowModelsResult(InfoBarSeverity::Error, F(L"Models_ImportFailed", { name, to_hstring(e.code().message()).c_str() }));
        }
        catch (std::runtime_error const& e)
        {
            ShowModelsResult(InfoBarSeverity::Error, std::string(e.what()) == "exists" ? F(L"Models_Exists", { name })
                                                                                        : F(L"Models_ImportFailed", { name, L"" }));
        }
    }

    void MainWindow::OnOpenModelsFolder(IInspectable const&, RoutedEventArgs const&)
    {
        ShellExecuteW(nullptr, L"open", ::AiUpscaler::ModelLibrary::Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ---- Unsaved configuration changes -----------------------------------------------------------

    bool MainWindow::HasUnsavedChanges()
    {
        return m_configTracking && !CollectSettings().SameConfiguration(m_savedConfig);
    }

    void MainWindow::UpdateConfigTitle()
    {
        if (!m_configTracking)
        {
            return;
        }
        bool unsaved = HasUnsavedChanges();
        std::wstring name = m_configPath.empty() ? (unsaved ? T(L"Config_Untitled") : L"") : fs::path(m_configPath).filename().wstring();
        if (!name.empty() && unsaved)
        {
            name += L"*";
        }
        ConfigTitleText().Text(name.empty() ? L"" : L"— " + name);
        Title(name.empty() ? L"AI Upscaler" : name + L" – AI Upscaler");
    }

    IAsyncOperation<bool> MainWindow::SaveConfigAsync(bool saveAs)
    {
        auto lifetime = get_strong();
        std::wstring path = m_configPath;
        if (saveAs || path.empty())
        {
            FileSavePicker picker(AppWindow().Id());
            picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
            picker.FileTypeChoices().Insert(T(L"Picker_Config"), single_threaded_vector<hstring>({ L".json" }));
            picker.SuggestedFileName(path.empty() ? T(L"Picker_ConfigName") : fs::path(path).stem().wstring());
            auto result = co_await picker.PickSaveFileAsync();
            if (!result)
            {
                co_return false;
            }
            path = result.Path();
        }

        auto settings = CollectSettings();
        try
        {
            settings.SaveConfiguration(path);
        }
        catch (std::exception const&)
        {
            ShowConfigMessage(InfoBarSeverity::Error, T(L"Config_SaveFailed"), path);
            co_return false;
        }
        ShowConfigMessage(InfoBarSeverity::Success, T(L"Config_Saved"), path);
        m_configPath = path;
        m_savedConfig = settings;
        UpdateConfigTitle();
        co_return true;
    }

    IAsyncOperation<bool> MainWindow::ConfirmUnsavedChangesAsync()
    {
        auto lifetime = get_strong();
        if (!HasUnsavedChanges())
        {
            co_return true;
        }
        if (m_savePromptOpen)
        {
            co_return false; // already asking (e.g. the close button pressed twice)
        }

        ContentDialog dialog;
        dialog.XamlRoot(Content().XamlRoot());
        dialog.Style(Application::Current().Resources().Lookup(box_value(L"DefaultContentDialogStyle")).as<Style>());
        dialog.RequestedTheme(RootGrid().ActualTheme());
        dialog.Title(box_value(m_configPath.empty() ? T(L"Unsaved_TitleNew") : F(L"Unsaved_Title", { fs::path(m_configPath).filename().wstring() })));
        TextBlock message;
        message.Text(T(L"Unsaved_Message"));
        message.TextWrapping(TextWrapping::Wrap);
        dialog.Content(message);
        dialog.PrimaryButtonText(T(L"Unsaved_Save"));
        dialog.SecondaryButtonText(T(L"Unsaved_DontSave"));
        dialog.CloseButtonText(T(L"Unsaved_Cancel"));
        dialog.DefaultButton(ContentDialogButton::Primary);

        m_savePromptOpen = true;
        auto promptClosed = wil::scope_exit([this] { m_savePromptOpen = false; });
        auto result = co_await dialog.ShowAsync();
        promptClosed.reset();
        if (result == ContentDialogResult::Primary)
        {
            co_return co_await SaveConfigAsync(false);
        }
        co_return result == ContentDialogResult::Secondary;
    }

    fire_and_forget MainWindow::CloseAfterConfirm()
    {
        auto lifetime = get_strong();
        if (co_await ConfirmUnsavedChangesAsync())
        {
            m_closeConfirmed = true;
            Close();
        }
    }

    // ---- Batch -------------------------------------------------------------------------------------

    void MainWindow::AddToBatch(std::vector<std::wstring> const& paths)
    {
        bool added = false;
        for (auto const& path : paths)
        {
            bool queued = std::any_of(m_batch.begin(), m_batch.end(), [&](BatchItem const& item) { return SamePath(item.Input, path); });
            if (!queued)
            {
                m_batch.push_back(BatchItem{ path });
                added = true;
            }
        }
        if (added)
        {
            RefreshBatchList();
        }
        ShowBottomTab(BatchTab());
    }

    void MainWindow::RefreshBatchList()
    {
        if (!BatchList())
        {
            return;
        }
        auto list = BatchList();
        list.Items().Clear();
        m_batchRows.clear();
        auto resources = Application::Current().Resources();
        auto caption = resources.Lookup(box_value(L"CaptionTextBlockStyle")).as<Style>();

        for (size_t i = 0; i < m_batch.size(); ++i)
        {
            Grid row;
            row.ColumnSpacing(12);
            row.Padding(ThicknessHelper::FromLengths(0, 4, 0, 4));
            for (auto width : { GridLengthHelper::Auto(), GridLengthHelper::FromValueAndType(1, GridUnitType::Star), GridLengthHelper::Auto(),
                                GridLengthHelper::Auto() })
            {
                ColumnDefinition column;
                column.Width(width);
                row.ColumnDefinitions().Append(column);
            }

            FontIcon icon;
            icon.FontSize(16);
            icon.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(icon);

            StackPanel text;
            text.VerticalAlignment(VerticalAlignment::Center);
            text.Spacing(2);
            TextBlock name;
            name.Text(fs::path(m_batch[i].Input).filename().wstring());
            name.TextTrimming(TextTrimming::CharacterEllipsis);
            TextBlock path;
            path.Style(caption);
            path.TextTrimming(TextTrimming::CharacterEllipsis);
            ProgressBar progress;
            progress.Maximum(100);
            text.Children().Append(name);
            text.Children().Append(path);
            text.Children().Append(progress);
            Grid::SetColumn(text, 1);
            row.Children().Append(text);

            TextBlock status;
            status.Style(caption);
            status.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(status, 2);
            row.Children().Append(status);

            Button remove;
            remove.Style(resources.Lookup(box_value(L"SubtleButtonStyle")).as<Style>());
            FontIcon removeIcon;
            removeIcon.Glyph(L""); // Cancel (×)
            removeIcon.FontSize(12);
            remove.Content(removeIcon);
            ToolTipService::SetToolTip(remove, box_value(T(L"Batch_RemoveTip")));
            remove.IsEnabled(!m_batchRunning);
            remove.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(remove, 3);
            auto input = m_batch[i].Input;
            remove.Click([this, input](auto&&, auto&&)
            {
                if (m_batchRunning)
                {
                    return;
                }
                std::erase_if(m_batch, [&](BatchItem const& item) { return item.Input == input; });
                RefreshBatchList();
            });
            row.Children().Append(remove);

            list.Items().Append(row);
            m_batchRows.push_back(BatchRow{ icon, status, path, progress });
            UpdateBatchRow(i);
        }

        BatchEmptyText().Visibility(m_batch.empty() ? Visibility::Visible : Visibility::Collapsed);
        auto done = std::count_if(m_batch.begin(), m_batch.end(), [](BatchItem const& item) { return item.State == BatchState::Done; });
        BatchSummaryText().Text(m_batch.empty() ? L"" : F(L"Batch_Summary", { std::to_wstring(m_batch.size()), std::to_wstring(done) }));
    }

    void MainWindow::UpdateBatchRow(size_t index)
    {
        if (index >= m_batchRows.size() || index >= m_batch.size())
        {
            return;
        }
        auto const& item = m_batch[index];
        auto& row = m_batchRows[index];
        switch (item.State)
        {
        case BatchState::Waiting:
            row.Icon.Glyph(L""); // Recent
            row.Status.Text(T(L"Batch_Waiting"));
            break;
        case BatchState::Running:
            row.Icon.Glyph(L""); // Play
            row.Status.Text(F(L"Batch_Running", { Num(item.Percent, 0) }));
            break;
        case BatchState::Done:
            row.Icon.Glyph(L""); // CheckMark
            row.Status.Text(item.Detail);
            break;
        case BatchState::Failed:
            row.Icon.Glyph(L""); // Error
            row.Status.Text(item.Detail);
            break;
        case BatchState::Cancelled:
            row.Icon.Glyph(L""); // Cancel
            row.Status.Text(T(L"Batch_Cancelled"));
            break;
        }
        row.Progress.Visibility(item.State == BatchState::Running ? Visibility::Visible : Visibility::Collapsed);
        row.Progress.Value(item.Percent);
        row.Path.Text(item.State == BatchState::Done && !item.Output.empty() ? F(L"Batch_SavedAs", { item.Output }) : item.Input);
    }

    std::wstring MainWindow::BatchOutputFor(size_t index)
    {
        auto const& input = m_batch[index].Input;
        fs::path base(DefaultOutputFor(input));
        auto output = base.wstring();
        // Two queued videos with the same name (from different folders) would otherwise write the same file.
        for (int n = 2;; ++n)
        {
            bool taken = SamePath(output, input);
            for (size_t j = 0; j < m_batch.size() && !taken; ++j)
            {
                taken = j != index && !m_batch[j].Output.empty() && SamePath(m_batch[j].Output, output);
            }
            if (!taken)
            {
                return output;
            }
            output = (base.parent_path() / (base.stem().wstring() + L"_" + std::to_wstring(n) + base.extension().wstring())).wstring();
        }
    }

    fire_and_forget MainWindow::OnBatchAdd(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        FileOpenPicker picker(AppWindow().Id());
        picker.SuggestedStartLocation(PickerLocationId::VideosLibrary);
        for (auto extension : VideoExtensions)
        {
            picker.FileTypeFilter().Append(extension);
        }
        auto results = co_await picker.PickMultipleFilesAsync();
        std::vector<std::wstring> paths;
        for (auto const& result : results)
        {
            paths.emplace_back(result.Path());
        }
        if (!paths.empty())
        {
            AddToBatch(paths);
        }
    }

    void MainWindow::OnBatchAddCurrent(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_inputPath.empty())
        {
            ShowResult(InfoBarSeverity::Warning, T(L"Err_NoVideo_Title"), T(L"Err_NoVideo"), false);
            return;
        }
        AddToBatch({ m_inputPath });
    }

    void MainWindow::OnBatchClear(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_batchRunning)
        {
            return;
        }
        m_batch.clear();
        RefreshBatchList();
    }

    void MainWindow::OnBatchDragOver(IInspectable const&, DragEventArgs const& e)
    {
        if (!m_batchRunning && e.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            e.AcceptedOperation(DataPackageOperation::Copy);
            e.DragUIOverride().Caption(T(L"Drag_AddToBatch"));
        }
    }

    fire_and_forget MainWindow::OnBatchDrop(IInspectable const&, DragEventArgs const& e)
    {
        auto lifetime = get_strong();
        auto view = e.DataView();
        if (m_batchRunning || !view.Contains(StandardDataFormats::StorageItems()))
        {
            co_return;
        }
        auto items = co_await view.GetStorageItemsAsync();
        std::vector<std::wstring> paths;
        for (auto const& item : items)
        {
            std::wstring path{ item.Path() };
            if (item.IsOfType(Windows::Storage::StorageItemTypes::Folder))
            {
                // A dropped folder adds the videos directly in it.
                std::vector<std::wstring> videos;
                std::error_code ec;
                for (auto const& entry : fs::directory_iterator(path, ec))
                {
                    if (entry.is_regular_file(ec) && IsVideoFile(entry.path().wstring()))
                    {
                        videos.push_back(entry.path().wstring());
                    }
                }
                std::sort(videos.begin(), videos.end());
                paths.insert(paths.end(), videos.begin(), videos.end());
            }
            else if (!IsConfigFile(path) && LowerExtension(path) != L".onnx")
            {
                paths.push_back(path);
            }
        }
        if (!paths.empty())
        {
            AddToBatch(paths);
        }
    }

    void MainWindow::OnBatchStart(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_job && m_job->IsRunning())
        {
            return;
        }
        ResultBar().IsOpen(false);
        if (m_batch.empty())
        {
            ShowResult(InfoBarSeverity::Warning, T(L"Err_BatchEmpty_Title"), T(L"Err_BatchEmpty"), false);
            return;
        }
        auto settings = CollectSettings();
        if (!settings.ModelPath.empty() && !fs::exists(settings.ModelPath))
        {
            ShowResult(InfoBarSeverity::Warning, T(L"Err_ModelMissing_Title"), T(L"Err_ModelMissing"), false);
            return;
        }
        if (!FindTools())
        {
            return;
        }

        // Videos already upscaled are skipped; when every one is, the whole list runs again.
        bool anyPending = std::any_of(m_batch.begin(), m_batch.end(), [](BatchItem const& item) { return item.State != BatchState::Done; });
        m_batchTotal = 0;
        for (auto& item : m_batch)
        {
            if (!anyPending || item.State != BatchState::Done)
            {
                item = BatchItem{ item.Input };
                ++m_batchTotal;
            }
        }
        m_batchNumber = 0;
        m_previewSeconds = 0; // a batch always upscales whole videos
        m_batchRunning = true;
        LogText().Text(L"");
        RefreshBatchList();
        StartNextBatchItem();
    }

    void MainWindow::StartNextBatchItem()
    {
        auto next = std::find_if(m_batch.begin(), m_batch.end(), [](BatchItem const& item) { return item.State == BatchState::Waiting; });
        if (next == m_batch.end())
        {
            FinishBatch();
            return;
        }
        auto index = static_cast<size_t>(next - m_batch.begin());
        auto& item = *next;
        ++m_batchNumber;
        if (!fs::exists(item.Input))
        {
            item.State = BatchState::Failed;
            item.Detail = T(L"Batch_NotFound");
            AppendLog(F(L"Log_BatchNotFound", { item.Input }));
            UpdateBatchRow(index);
            StartNextBatchItem();
            return;
        }

        item.Output = BatchOutputFor(index);
        item.State = BatchState::Running;
        item.Percent = 0;
        m_batchCurrent = index;
        UpdateBatchRow(index);
        AppendLog(L"== " + item.Input + L" → " + item.Output);
        if (!StartUpscale(item.Input, item.Output))
        {
            item.State = BatchState::Waiting;
            m_batchCurrent.reset();
            m_batchRunning = false;
            SetRunning(false);
        }
    }

    void MainWindow::OnBatchItemExited(::AiUpscaler::JobOutcome outcome, DWORD exitCode, std::wstring const& elapsed)
    {
        auto index = *m_batchCurrent;
        m_batchCurrent.reset();
        auto& item = m_batch[index];
        switch (outcome)
        {
        case ::AiUpscaler::JobOutcome::Completed:
            item.State = BatchState::Done;
            item.Percent = 100;
            item.Detail = F(L"Batch_Done", { elapsed });
            break;
        case ::AiUpscaler::JobOutcome::Cancelled:
            item.State = BatchState::Cancelled;
            break;
        case ::AiUpscaler::JobOutcome::Failed:
            item.State = BatchState::Failed;
            item.Detail = F(L"Batch_Failed", { std::to_wstring(exitCode) });
            break;
        }
        UpdateBatchRow(index);

        if (outcome == ::AiUpscaler::JobOutcome::Cancelled)
        {
            // Cancel stops the whole batch; the videos not yet upscaled stay in the list.
            m_batchRunning = false;
            SetRunning(false);
            JobProgress().Value(0);
            StatusText().Text(T(L"Status_BatchCancelled"));
            return;
        }
        StartNextBatchItem();
    }

    void MainWindow::FinishBatch()
    {
        m_batchRunning = false;
        SetRunning(false);
        JobProgress().Value(100);

        auto done = std::count_if(m_batch.begin(), m_batch.end(), [](BatchItem const& item) { return item.State == BatchState::Done; });
        auto failed = std::count_if(m_batch.begin(), m_batch.end(), [](BatchItem const& item) { return item.State == BatchState::Failed; });
        // Open shows the folder of the last finished video.
        std::wstring folder;
        for (auto const& item : m_batch)
        {
            if (item.State == BatchState::Done)
            {
                folder = fs::path(item.Output).parent_path().wstring();
            }
        }
        m_runningOutput = folder;

        auto result = F(L"Batch_Result", { std::to_wstring(done), std::to_wstring(m_batch.size()) });
        StatusText().Text(result);
        if (failed == 0)
        {
            ShowResult(InfoBarSeverity::Success, T(L"Batch_Complete_Title"), result, !folder.empty());
        }
        else
        {
            ShowResult(InfoBarSeverity::Warning, T(L"Batch_Partial_Title"), result + L" " + T(L"Err_SeeLog"), !folder.empty());
            LogExpander().IsExpanded(true);
        }
    }

    // ---- Upscale run ---------------------------------------------------------------------------------

    void MainWindow::OnStart(IInspectable const&, RoutedEventArgs const&)
    {
        ResultBar().IsOpen(false);
        std::wstring output{ OutputBox().Text() };
        auto settings = CollectSettings();

        if (m_inputPath.empty() || !fs::exists(m_inputPath))
        {
            ShowResult(InfoBarSeverity::Warning, T(L"Err_NoVideo_Title"), T(L"Err_NoVideo"), false);
            return;
        }
        if (output.empty())
        {
            ShowBottomTab(UpscaleSettingsTab());
            OutputBox().Focus(FocusState::Programmatic);
            return;
        }
        if (!settings.ModelPath.empty() && !fs::exists(settings.ModelPath))
        {
            ShowResult(InfoBarSeverity::Warning, T(L"Err_ModelMissing_Title"), T(L"Err_ModelMissing"), false);
            return;
        }
        // Previews go next to the real output so they never overwrite a finished video.
        m_previewSeconds = _wtoi(SelectedTag(PreviewCombo()).c_str());
        if (m_previewSeconds > 0)
        {
            output = PreviewPathFor(output);
        }
        LogText().Text(L"");
        StartUpscale(m_inputPath, output);
    }

    bool MainWindow::StartUpscale(std::wstring const& input, std::wstring const& output)
    {
        auto settings = CollectSettings();
        auto tools = FindTools();
        if (!tools)
        {
            return false;
        }

        settings.Save();

        // The GPU can only do one thing at a time well: stop any frame preview first.
        m_scrubTimer.Stop();
        m_aiPreviewTimer.Stop();
        m_pendingPreviewArgs.reset();
        if (m_previewJob)
        {
            m_previewJob->Terminate();
            m_previewJob.reset();
        }
        SourceBusy().IsActive(false);
        OutputBusy().IsActive(false);

        std::error_code ec;
        fs::create_directories(fs::path(output).parent_path(), ec);

        std::vector<std::wstring> args{ input, L"--output", output, L"--device", settings.Device, L"--codec", settings.Codec,
                                        L"--crf", std::to_wstring(::AiUpscaler::Settings::QualityToCrf(settings.Quality)),
                                        L"--tile", std::to_wstring(settings.TileSize),
                                        L"--ffmpeg", tools->Ffmpeg.wstring(), L"--ffprobe", tools->Ffprobe.wstring(), L"--json" };
        // Live previews are shown in the studio panes, which are sized for the video open there.
        bool livePreviews = !m_inputPath.empty() && SamePath(input, m_inputPath);
        if (livePreviews)
        {
            args.insert(args.end(), { L"--preview-dir", m_previewDir.wstring(), L"--preview-interval", L"2" });
        }
        if (!settings.ModelPath.empty())
        {
            args.insert(args.end(), { L"--model", settings.ModelPath });
        }
        if (settings.TargetHeight > 0)
        {
            // A real 16:9 frame (e.g. 1920x1080); --fit decides how other shapes fill it.
            auto size = std::to_wstring(FrameWidthFor(settings.TargetHeight)) + L"x" + std::to_wstring(settings.TargetHeight);
            args.insert(args.end(), { L"--size", size, L"--fit", settings.Fit.empty() ? L"pad" : settings.Fit });
        }
        if (!settings.KeepAudio)
        {
            args.emplace_back(L"--no-audio");
        }
        else if (auto audio = settings.AudioEnhancements(); !audio.empty())
        {
            args.insert(args.end(), { L"--audio-enhance", audio });
        }
        if (m_previewSeconds > 0)
        {
            args.insert(args.end(), { L"--max-seconds", std::to_wstring(m_previewSeconds) });
        }

        auto onUi = [weak = get_weak(), dispatcher = DispatcherQueue()](auto action)
        {
            dispatcher.TryEnqueue([weak, action]
            {
                if (auto self = weak.get())
                {
                    action(*self);
                }
            });
        };
        ::AiUpscaler::EngineCallbacks callbacks;
        callbacks.Started = [onUi](::AiUpscaler::JobStarted const& s) { onUi([s](MainWindow& w) { w.OnJobStarted(s); }); };
        callbacks.Progress = [onUi](::AiUpscaler::JobProgress const& p) { onUi([p](MainWindow& w) { w.OnJobProgress(p); }); };
        callbacks.Preview = [onUi](::AiUpscaler::JobPreview const& p) { onUi([p](MainWindow& w) { w.OnPreview(p); }); };
        callbacks.Log = [onUi](std::wstring const& line) { onUi([line](MainWindow& w) { w.AppendLog(line); }); };
        callbacks.Exited = [onUi](::AiUpscaler::JobOutcome outcome, DWORD code) { onUi([outcome, code](MainWindow& w) { w.OnJobExited(outcome, code); }); };

        m_runningOutput = output;
        m_jobStart = std::chrono::steady_clock::now();
        m_job = std::make_unique<::AiUpscaler::EngineJob>(tools->Engine, std::move(args), std::move(callbacks));
        try
        {
            m_job->Start();
        }
        catch (...)
        {
            m_job.reset();
            ShowResult(InfoBarSeverity::Error, T(L"Err_EngineStart"), std::wstring(to_message()), false);
            return false;
        }

        m_sourceFrames = 0;
        m_rateStartFrame = -1;
        m_measuredFps = 0;
        EstimateText().Visibility(Visibility::Collapsed);
        SetRunning(true);
        if (!m_batchRunning)
        {
            ShowBottomTab(ProgressTab()); // a batch shows each video's progress on its own tab
        }
        StatusText().Text(T(L"Status_Loading"));
        JobInfoText().Text(L"");
        JobProgress().IsIndeterminate(true);
        if (livePreviews)
        {
            OutputPlaceholderText().Text(T(L"Placeholder_Live"));
        }
        else
        {
            OutputPlaceholder().Visibility(OutputImage().Source() ? Visibility::Collapsed : Visibility::Visible);
            OutputPlaceholderText().Text(F(L"Placeholder_BatchOther", { fs::path(input).filename().wstring() }));
        }
        return true;
    }

    void MainWindow::OnCancel(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_job && m_job->IsRunning())
        {
            m_job->Cancel();
            m_cancelRequested = true;
            CancelButton().IsEnabled(false);
            StatusText().Text(T(L"Status_Cancelling"));

            // The engine stops after the frame it is on; give a slow model time to finish it.
            if (!m_cancelTimer)
            {
                m_cancelTimer = DispatcherQueue().CreateTimer();
                m_cancelTimer.Interval(std::chrono::seconds(15));
                m_cancelTimer.IsRepeating(false);
                m_cancelTimer.Tick([this](auto&&, auto&&)
                {
                    if (m_job && m_job->IsRunning())
                    {
                        AppendLog(L"The engine did not stop after Cancel; ending it.");
                        m_job->Terminate();
                    }
                });
            }
            m_cancelTimer.Start();
        }
    }

    void MainWindow::OnOpenOutput(IInspectable const&, RoutedEventArgs const&)
    {
        // Opens the video in the default player.
        ShellExecuteW(nullptr, L"open", m_runningOutput.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void MainWindow::OnJobStarted(::AiUpscaler::JobStarted const& s)
    {
        JobInfoText().Text(F(L"JobInfo", { std::to_wstring(s.Width), std::to_wstring(s.Height), std::to_wstring(s.OutWidth), std::to_wstring(s.OutHeight),
                                           std::to_wstring(s.FinalWidth), std::to_wstring(s.FinalHeight), Num(s.Fps, 2), s.Encoder,
                                           T(s.AudioImproved ? L"Audio_ImprovedInfo" : s.Audio ? L"Audio_Kept" : L"Audio_None") }));
        if (s.Height > 0)
        {
            m_modelScale = s.OutHeight / s.Height;
        }
        StatusText().Text(BatchPrefix() + T(m_previewSeconds > 0 ? L"Status_UpscalingPreview" : L"Status_Upscaling"));
        m_sourceFrames = s.SourceFrames;
    }

    std::wstring MainWindow::BatchPrefix() const
    {
        if (!m_batchCurrent)
        {
            return L"";
        }
        return F(L"Batch_Item", { std::to_wstring(m_batchNumber), std::to_wstring(m_batchTotal),
                                  fs::path(m_batch[*m_batchCurrent].Input).filename().wstring() }) + L"\n";
    }

    void MainWindow::UpdateEstimate(::AiUpscaler::JobProgress const& p)
    {
        // Measure from the first progress event so the slow first frame (GPU shader
        // compilation) does not skew the rate, then wait for a few frames before estimating.
        constexpr int64_t FramesBeforeEstimate = 3;
        auto now = std::chrono::steady_clock::now();
        if (m_rateStartFrame < 0)
        {
            m_rateStartFrame = p.Frame;
            m_rateStartTime = now;
            return;
        }
        auto frames = p.Frame - m_rateStartFrame;
        auto seconds = std::chrono::duration<double>(now - m_rateStartTime).count();
        if (frames < FramesBeforeEstimate || seconds <= 0)
        {
            return;
        }
        m_measuredFps = static_cast<double>(frames) / seconds;

        std::wstring text;
        if (p.Total > 0)
        {
            auto left = static_cast<double>(p.Total - p.Frame) / m_measuredFps;
            text = F(L"Estimate_Left", { FormatLongDuration(left), FinishTime(left) });
        }
        if (m_previewSeconds > 0 && m_sourceFrames > 0)
        {
            text += (text.empty() ? L"" : L"\n");
            text += F(L"Estimate_Whole", { FormatLongDuration(static_cast<double>(m_sourceFrames) / m_measuredFps) });
        }
        if (!text.empty())
        {
            EstimateText().Text(text);
            EstimateText().Visibility(Visibility::Visible);
        }
    }

    void MainWindow::OnJobProgress(::AiUpscaler::JobProgress const& p)
    {
        if (p.Total > 0)
        {
            JobProgress().IsIndeterminate(false);
            JobProgress().Value(100.0 * static_cast<double>(p.Frame) / static_cast<double>(p.Total));
            if (m_batchCurrent)
            {
                m_batch[*m_batchCurrent].Percent = 100.0 * static_cast<double>(p.Frame) / static_cast<double>(p.Total);
                UpdateBatchRow(*m_batchCurrent);
            }
        }

        auto elapsed = FormatDuration(std::chrono::duration<double>(std::chrono::steady_clock::now() - m_jobStart).count());
        auto fps = Num(p.Fps, 2);
        std::wstring status;
        if (p.Total > 0 && p.EtaSeconds)
        {
            status = F(L"Progress_FramesEta", { std::to_wstring(p.Frame), std::to_wstring(p.Total), fps, elapsed, FormatDuration(*p.EtaSeconds) });
        }
        else if (p.Total > 0)
        {
            status = F(L"Progress_Frames", { std::to_wstring(p.Frame), std::to_wstring(p.Total), fps, elapsed });
        }
        else
        {
            status = F(L"Progress_FramesOnly", { std::to_wstring(p.Frame), fps, elapsed });
        }
        StatusText().Text(BatchPrefix() + status);
        UpdateEstimate(p);
    }

    void MainWindow::OnJobExited(::AiUpscaler::JobOutcome outcome, DWORD exitCode)
    {
        m_job.reset();
        if (m_cancelTimer)
        {
            m_cancelTimer.Stop();
        }
        // An engine ended by the cancel timer exits with an error code; it was still a cancel,
        // and its partial file is left behind.
        if (std::exchange(m_cancelRequested, false) && outcome == ::AiUpscaler::JobOutcome::Failed)
        {
            outcome = ::AiUpscaler::JobOutcome::Cancelled;
            DeleteFileW(m_runningOutput.c_str());
        }
        EstimateText().Visibility(Visibility::Collapsed);
        JobProgress().IsIndeterminate(false);

        auto elapsed = FormatDuration(std::chrono::duration<double>(std::chrono::steady_clock::now() - m_jobStart).count());
        if (m_batchCurrent)
        {
            OnBatchItemExited(outcome, exitCode, elapsed);
            return;
        }
        SetRunning(false);
        switch (outcome)
        {
        case ::AiUpscaler::JobOutcome::Completed:
            JobProgress().Value(100);
            StatusText().Text(F(L"Status_Finished", { elapsed }));
            if (m_previewSeconds > 0)
            {
                std::wstring message = m_runningOutput;
                if (m_measuredFps > 0 && m_sourceFrames > 0)
                {
                    message += L"\n" + F(L"Estimate_Whole", { FormatLongDuration(static_cast<double>(m_sourceFrames) / m_measuredFps) });
                }
                ShowResult(InfoBarSeverity::Success, T(L"Result_PreviewReady"), message, true);
            }
            else
            {
                ShowResult(InfoBarSeverity::Success, T(L"Result_Complete"), m_runningOutput, true);
            }
            break;
        case ::AiUpscaler::JobOutcome::Cancelled:
            JobProgress().Value(0);
            StatusText().Text(T(L"Status_Cancelled"));
            break;
        case ::AiUpscaler::JobOutcome::Failed:
            StatusText().Text(F(L"Status_Failed", { std::to_wstring(exitCode) }));
            ShowResult(InfoBarSeverity::Error, T(L"Err_UpscaleFailed_Title"), T(L"Err_SeeLog"), false);
            LogExpander().IsExpanded(true);
            break;
        }
    }

    void MainWindow::SetRunning(bool running)
    {
        for (Control control : std::initializer_list<Control>{ OpenSourceButton(), LoadConfigButton(), ModelCombo(), ManageModelsButton(), OutputBox(), OutputButton(),
                                                               DeviceCombo(), HeightCombo(), CodecCombo(), QualitySlider(), TileBox(),
                                                               AudioToggle(), AudioImproveButton(), FormatCombo(), PreviewCombo(), StartButton(), ImportModelButton() })
        {
            control.IsEnabled(!running);
        }
        FitCombo().IsEnabled(!running && SelectedTag(HeightCombo()) != L"0");
        AudioImproveButton().IsEnabled(!running && AudioToggle().IsOn());
        bool haveSource = !running && m_duration > 0;
        TimelineSlider().IsEnabled(haveSource);
        AutoPreviewCheck().IsEnabled(!running);
        FilmFrames().IsHitTestVisible(haveSource);
        if (running)
        {
            m_aiPreviewTimer.Stop();
            AutoPreviewStatus().Text(T(L"Auto_Live"));
        }
        else
        {
            AutoPreviewStatus().Text(L"");
        }
        CancelButton().IsEnabled(running);
        SourcePane().AllowDrop(!running);
        BatchStartButton().IsEnabled(!running);
        // The queue can grow while a single video upscales, but not change under a running batch.
        for (Control control : std::initializer_list<Control>{ BatchAddButton(), BatchAddCurrentButton(), BatchClearButton() })
        {
            control.IsEnabled(!m_batchRunning);
        }
        BatchPanel().AllowDrop(!m_batchRunning);
        RefreshBatchList();
    }

    void MainWindow::AppendLog(std::wstring const& line)
    {
        // Follow the end only when already there, so reading earlier lines isn't interrupted.
        auto scroll = LogScroll();
        bool atBottom = scroll.VerticalOffset() >= scroll.ScrollableHeight() - 4;

        std::wstring text{ LogText().Text() };
        if (!text.empty())
        {
            text += L"\n";
        }
        text += line;
        LogText().Text(text);

        if (atBottom)
        {
            scroll.UpdateLayout();
            scroll.ChangeView(nullptr, scroll.ScrollableHeight(), nullptr, true);
        }
    }

    void MainWindow::OnCopyLog(IInspectable const&, RoutedEventArgs const&)
    {
        Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(LogText().Text());
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);

        // Brief tick as confirmation, then back to the copy icon.
        CopyLogIcon().Glyph(L"");
        if (!m_copyLogTimer)
        {
            m_copyLogTimer = DispatcherQueue().CreateTimer();
            m_copyLogTimer.Interval(std::chrono::milliseconds(1500));
            m_copyLogTimer.IsRepeating(false);
            m_copyLogTimer.Tick([this](auto&&, auto&&) { CopyLogIcon().Glyph(L""); });
        }
        m_copyLogTimer.Stop();
        m_copyLogTimer.Start();
    }

    void MainWindow::ShowResult(InfoBarSeverity severity, std::wstring const& title, std::wstring const& message, bool canOpenOutput)
    {
        ResultBar().Severity(severity);
        ResultBar().Title(title);
        ResultBar().Message(message);
        OpenOutputButton().Visibility(canOpenOutput ? Visibility::Visible : Visibility::Collapsed);
        ResultBar().IsOpen(true);
        ShowBottomTab(ProgressTab()); // the InfoBar lives on the Progress tab
    }
}
