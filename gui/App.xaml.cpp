#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Settings.h"
#include "Strings.h"

#include <shellapi.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace winrt::AiUpscaler::implementation
{
    App::App()
    {
        // The UI language must be chosen before any XAML or resource is loaded.
        ::AiUpscaler::Strings::ApplyLanguage(::AiUpscaler::Settings::Load().Language);

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        m_window = make<MainWindow>();
        m_window.Activate();

        // AiUpscaler.exe <video>: open it straight away.
        int argc = 0;
        if (auto argv = CommandLineToArgvW(GetCommandLineW(), &argc))
        {
            if (argc > 1)
            {
                get_self<MainWindow>(m_window.as<AiUpscaler::MainWindow>())->OpenFile(argv[1]);
            }
            LocalFree(argv);
        }
    }
}
