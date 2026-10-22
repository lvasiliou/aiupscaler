#pragma once

#include "MainWindow.g.h"
#include "EngineJob.h"
#include "Settings.h"

namespace winrt::AiUpscaler::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        /// Opens a video or configuration file passed on the command line (e.g. "Open with").
        void OpenFile(std::wstring const& path);

        void OnTileChanged(Microsoft::UI::Xaml::Controls::NumberBox const&, Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);

        // Batch
        winrt::fire_and_forget OnBatchAdd(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnBatchAddCurrent(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnBatchClear(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnBatchStart(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnBatchDragOver(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        winrt::fire_and_forget OnBatchDrop(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);

        // Studio
        winrt::fire_and_forget OnBrowseInput(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnBrowseOutput(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnManageModels(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnQualityChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void OnStart(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCancel(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenOutput(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        void OnSourceDragEnter(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        void OnSourceDragOver(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        void OnSourceDragLeave(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        winrt::fire_and_forget OnSourceDrop(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);

        void OnTimelineChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void OnZoomToggle(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAutoPreviewChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnPreviewAccelerator(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&, Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const&);
        void OnOpenAccelerator(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&, Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const&);
        void OnScrollerSizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void OnSourceViewChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const&);
        void OnOutputViewChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const&);
        void OnBottomTabChanged(Microsoft::UI::Xaml::Controls::SelectorBar const&, Microsoft::UI::Xaml::Controls::SelectorBarSelectionChangedEventArgs const&);
        void OnSettingChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnFilmStripSizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);

        // App settings page
        void OnOpenAppSettings(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSettingsBack(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSettingsEscape(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&, Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const&);
        void OnSettingsNavChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnThemeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnLanguageChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        winrt::fire_and_forget OnRestart(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOutputLocationChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnChooseOutputFolder(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnLoadConfiguration(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnSaveConfiguration(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnSaveConfigurationAs(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAudioOptionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCopyLog(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnImportModel(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenModelsFolder(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        struct Tools
        {
            std::filesystem::path Engine;
            std::filesystem::path Ffmpeg;
            std::filesystem::path Ffprobe;
        };

        void InitializeWindow();
        void ApplySettings(::AiUpscaler::Settings const& settings);
        ::AiUpscaler::Settings CollectSettings();
        std::optional<Tools> FindTools();

        // Configuration files and unsaved changes
        void LoadConfigurationFile(std::filesystem::path const& path);
        winrt::fire_and_forget LoadConfigurationAfterConfirm(std::wstring path);
        bool HasUnsavedChanges();
        /// Shows the configuration's name, with * for unsaved changes, in the title bar.
        void UpdateConfigTitle();
        /// Saves to the loaded configuration file, or asks for a new one (always, with saveAs). False if not saved.
        Windows::Foundation::IAsyncOperation<bool> SaveConfigAsync(bool saveAs);
        /// With unsaved changes, asks whether to save them. False if the user cancelled (or saving didn't happen).
        Windows::Foundation::IAsyncOperation<bool> ConfirmUnsavedChangesAsync();
        winrt::fire_and_forget CloseAfterConfirm();

        // App settings
        void LoadAppSettingsPage();
        void ShowSettingsSection(std::wstring const& tag);
        void ApplyTheme(std::wstring const& theme);
        /// Caption button glyphs use the accent colour in light theme, the defaults in dark.
        void UpdateCaptionButtons();
        void SaveAppSettings();
        std::wstring DefaultOutputFor(std::wstring const& input) const;

        // Model library
        void RefreshModels(std::wstring const& select);
        void PopulateModelsList();
        void ShowModelsResult(Microsoft::UI::Xaml::Controls::InfoBarSeverity severity, std::wstring const& message);

        void SetInput(std::wstring const& path);
        void StartPreviewJob(std::vector<std::wstring> args, bool aiFrame);
        void RequestSourceFrame(double seconds);
        /// AI-upscales the frame at the timeline position now.
        void RequestAiPreview();
        /// (Re)starts the 3-second wait before an automatic AI preview, if they are enabled.
        void ScheduleAiPreview();
        void SetZoom(bool actualPixels);
        void ShowZoomState(bool actualPixels);

        /// Starts the engine on `input`; live previews only when it is the video open in the studio.
        bool StartUpscale(std::wstring const& input, std::wstring const& output);

        // Batch
        void AddToBatch(std::vector<std::wstring> const& paths);
        void RefreshBatchList();
        void UpdateBatchRow(size_t index);
        std::wstring BatchOutputFor(size_t index) const;
        /// Starts the next waiting video; finishes the batch when none is left.
        void StartNextBatchItem();
        void OnBatchItemExited(::AiUpscaler::JobOutcome outcome, DWORD exitCode, std::wstring const& elapsed);
        void FinishBatch();
        /// "Video 2 of 5: name.mkv" and a line break while a batch runs; empty otherwise.
        std::wstring BatchPrefix() const;

        void SetRunning(bool running);
        void SetTimelinePosition(double seconds);
        void AppendLog(std::wstring const& line);
        void ShowResult(Microsoft::UI::Xaml::Controls::InfoBarSeverity severity, std::wstring const& title, std::wstring const& message, bool canOpenOutput);
        void ShowBottomTab(Microsoft::UI::Xaml::Controls::SelectorBarItem const& tab);
        void UpdateAudioButton();
        void ShowConfigMessage(Microsoft::UI::Xaml::Controls::InfoBarSeverity severity, std::wstring const& title, std::wstring const& message);

        winrt::fire_and_forget LoadImage(Microsoft::UI::Xaml::Controls::Image target, std::wstring path, uint64_t generation, bool isSource);
        void FitImages();
        void SyncView(Microsoft::UI::Xaml::Controls::ScrollViewer const& from, Microsoft::UI::Xaml::Controls::ScrollViewer const& to,
                      Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const& e);

        // Film strip
        void StartThumbnails();
        void BuildFilmFrames(int count);
        void OnThumbnail(Windows::Data::Json::JsonObject const& thumbnail);
        winrt::fire_and_forget LoadThumbnail(Microsoft::UI::Xaml::Controls::Image target, std::wstring path);
        void LayoutSprockets();
        void UpdatePlayhead();

        // Hardware capabilities and output description
        void StartCapabilities();
        void OnCapabilities(Windows::Data::Json::JsonObject const& capabilities);
        void SetItemAvailable(Microsoft::UI::Xaml::Controls::ComboBox const& combo, std::wstring const& tag, bool available,
                              std::wstring const& label, std::wstring const& reason);
        std::wstring DescribeFinalSize(::AiUpscaler::Settings const& settings, bool withNote);
        void UpdateOutputSummary();

        void OnProbe(::AiUpscaler::JobProbe const& probe);
        void OnPreview(::AiUpscaler::JobPreview const& preview);
        void OnPreviewJobExited(::AiUpscaler::JobOutcome outcome, DWORD exitCode);
        void OnJobStarted(::AiUpscaler::JobStarted const& started);
        void OnJobProgress(::AiUpscaler::JobProgress const& progress);
        void UpdateEstimate(::AiUpscaler::JobProgress const& progress);
        void OnJobExited(::AiUpscaler::JobOutcome outcome, DWORD exitCode);

        // App-level settings (theme, language, output folder); the studio controls hold the rest.
        ::AiUpscaler::Settings m_appSettings;
        std::wstring m_startupLanguage;
        // Accent colour source; its change event keeps the caption buttons in sync.
        Windows::UI::ViewManagement::UISettings m_uiSettings;
        Windows::UI::ViewManagement::UISettings::ColorValuesChanged_revoker m_colorValuesChanged;
        bool m_loadingAppSettings{};
        bool m_refreshingModels{};
        bool m_refreshingItems{}; // re-selecting a combo item only to refresh its displayed text

        // Main upscale job.
        std::unique_ptr<::AiUpscaler::EngineJob> m_job;
        std::wstring m_runningOutput;
        std::chrono::steady_clock::time_point m_jobStart;
        int m_previewSeconds{};
        int64_t m_sourceFrames{};
        // Speed is measured from the first progress event on, so model warm-up is excluded.
        int64_t m_rateStartFrame{ -1 };
        std::chrono::steady_clock::time_point m_rateStartTime;
        double m_measuredFps{};

        // Configuration: the file it was last loaded from or saved to (empty = never saved),
        // and its contents then; anything different is an unsaved change.
        std::wstring m_configPath;
        ::AiUpscaler::Settings m_savedConfig; // compared with SameConfiguration
        bool m_configTracking{}; // false until the controls hold the startup settings
        bool m_savePromptOpen{};
        bool m_closeConfirmed{};

        // Batch queue, upscaled one video at a time with the current upscale settings.
        enum class BatchState
        {
            Waiting,
            Running,
            Done,
            Failed,
            Cancelled,
        };
        struct BatchItem
        {
            std::wstring Input;
            std::wstring Output;
            BatchState State{ BatchState::Waiting };
            double Percent{};
            std::wstring Detail;
        };
        struct BatchRow
        {
            Microsoft::UI::Xaml::Controls::FontIcon Icon;
            Microsoft::UI::Xaml::Controls::TextBlock Status;
            Microsoft::UI::Xaml::Controls::TextBlock Path;
            Microsoft::UI::Xaml::Controls::ProgressBar Progress;
        };
        std::vector<BatchItem> m_batch;
        std::vector<BatchRow> m_batchRows;
        bool m_batchRunning{};
        std::optional<size_t> m_batchCurrent;
        int m_batchTotal{}; // videos in this run, for "Video 2 of 5"
        int m_batchNumber{};

        // Probe and single-frame preview jobs: one at a time, and a newer request replaces
        // any that is still waiting (so fast scrubbing doesn't queue up stale frames).
        std::unique_ptr<::AiUpscaler::EngineJob> m_previewJob;
        bool m_previewJobIsAi{};
        std::optional<std::vector<std::wstring>> m_pendingPreviewArgs;
        bool m_pendingPreviewIsAi{};
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_scrubTimer{ nullptr };
        // Fires 3 s after the frame last changed, so dragging the timeline doesn't start an
        // AI preview for every position passed on the way.
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_aiPreviewTimer{ nullptr };
        // Kills the engine if it hasn't stopped some time after Cancel, so the app never
        // stays stuck on "Cancelling".
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_cancelTimer{ nullptr };
        bool m_cancelRequested{ false };
        bool m_applyingSettings{ false };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_copyLogTimer{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_configBarTimer{ nullptr };

        // Film strip thumbnails and hardware probe run alongside frame previews.
        std::unique_ptr<::AiUpscaler::EngineJob> m_thumbJob;
        std::unique_ptr<::AiUpscaler::EngineJob> m_capsJob;
        std::vector<double> m_thumbTimes;

        // Source geometry (from --probe) and the model's scale (learned from a preview).
        int m_sourceWidth{};
        int m_sourceHeight{};
        double m_sar{ 1.0 };
        int m_modelScale{};

        std::wstring m_inputPath;
        std::wstring m_autoOutput;
        std::filesystem::path m_previewDir;
        double m_duration{};
        int m_outputPixelWidth{};
        int m_outputPixelHeight{};
        uint64_t m_imageGeneration{};
        uint64_t m_shownSourceGeneration{};
        uint64_t m_shownOutputGeneration{};
        bool m_updatingTimeline{};
        // The pane we last moved in code; its echo ViewChanged events are not mirrored back.
        Microsoft::UI::Xaml::Controls::ScrollViewer m_programmaticScroll{ nullptr };
    };
}

namespace winrt::AiUpscaler::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
