#include "pch.h"
#include "EngineJob.h"

using namespace winrt::Windows::Data::Json;

namespace AiUpscaler
{
    namespace
    {
        std::wstring Utf8ToWide(std::string const& text)
        {
            if (text.empty())
            {
                return {};
            }
            int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring wide(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
            return wide;
        }

        /// Quotes one argument per the CommandLineToArgvW rules.
        void AppendQuoted(std::wstring& commandLine, std::wstring const& argument)
        {
            if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
            {
                commandLine += argument;
                return;
            }
            commandLine += L'"';
            for (auto it = argument.begin();; ++it)
            {
                size_t backslashes = 0;
                while (it != argument.end() && *it == L'\\')
                {
                    ++it;
                    ++backslashes;
                }
                if (it == argument.end())
                {
                    commandLine.append(backslashes * 2, L'\\');
                    break;
                }
                if (*it == L'"')
                {
                    commandLine.append(backslashes * 2 + 1, L'\\');
                }
                else
                {
                    commandLine.append(backslashes, L'\\');
                }
                commandLine += *it;
            }
            commandLine += L'"';
        }

        double NumberOr(JsonObject const& obj, wchar_t const* key, double fallback)
        {
            auto value = obj.TryLookup(key);
            return value && value.ValueType() == JsonValueType::Number ? value.GetNumber() : fallback;
        }

        /// String value, or empty when missing or null.
        std::wstring StringOr(JsonObject const& obj, wchar_t const* key)
        {
            auto value = obj.TryLookup(key);
            return value && value.ValueType() == JsonValueType::String ? std::wstring(value.GetString()) : std::wstring{};
        }

        void CreateInheritablePipe(wil::unique_handle& read, wil::unique_handle& write, bool childReads)
        {
            SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
            THROW_IF_WIN32_BOOL_FALSE(CreatePipe(read.put(), write.put(), &security, 0));
            // Only the child's end may be inherited.
            HANDLE parentEnd = childReads ? write.get() : read.get();
            THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(parentEnd, HANDLE_FLAG_INHERIT, 0));
        }
    }

    EngineJob::EngineJob(std::filesystem::path enginePath, std::vector<std::wstring> arguments, EngineCallbacks callbacks)
        : m_enginePath(std::move(enginePath)), m_arguments(std::move(arguments)), m_callbacks(std::move(callbacks))
    {
    }

    EngineJob::~EngineJob()
    {
        if (m_running)
        {
            Cancel();
            if (m_process && WaitForSingleObject(m_process.get(), 5000) == WAIT_TIMEOUT && m_job)
            {
                TerminateJobObject(m_job.get(), 1);
            }
        }
        for (auto* thread : { &m_waitThread, &m_stdoutThread, &m_stderrThread })
        {
            if (thread->joinable())
            {
                thread->join();
            }
        }
    }

    void EngineJob::Start()
    {
        wil::unique_handle stdinRead, stdoutWrite, stderrWrite;
        CreateInheritablePipe(stdinRead, m_stdinWrite, true);
        CreateInheritablePipe(m_stdoutRead, stdoutWrite, false);
        CreateInheritablePipe(m_stderrRead, stderrWrite, false);

        m_job.reset(CreateJobObjectW(nullptr, nullptr));
        THROW_LAST_ERROR_IF_NULL(m_job);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(m_job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));

        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdinRead.get();
        startup.hStdOutput = stdoutWrite.get();
        startup.hStdError = stderrWrite.get();

        auto commandLine = BuildCommandLine(m_enginePath, m_arguments);
        auto workingDir = m_enginePath.parent_path().wstring();
        PROCESS_INFORMATION info{};
        THROW_IF_WIN32_BOOL_FALSE(CreateProcessW(
            m_enginePath.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
            nullptr, workingDir.c_str(), &startup, &info));
        m_process.reset(info.hProcess);
        wil::unique_handle thread(info.hThread);

        // Assign before the engine runs so every ffmpeg it spawns joins the job too.
        THROW_IF_WIN32_BOOL_FALSE(AssignProcessToJobObject(m_job.get(), m_process.get()));
        ResumeThread(thread.get());
        m_running = true;

        // Close our copies of the child's ends so reads see EOF when it exits.
        stdinRead.reset();
        stdoutWrite.reset();
        stderrWrite.reset();

        m_stdoutThread = std::thread([this] { ReadStdout(); });
        m_stderrThread = std::thread([this] { ReadStderr(); });
        m_waitThread = std::thread([this] { WaitForExit(); });
    }

    void EngineJob::Cancel()
    {
        std::scoped_lock lock(m_stdinLock);
        if (!m_stdinWrite)
        {
            return;
        }
        static constexpr char command[] = "cancel\n";
        DWORD written = 0;
        WriteFile(m_stdinWrite.get(), command, sizeof(command) - 1, &written, nullptr);
    }

    void EngineJob::Terminate()
    {
        if (m_job)
        {
            TerminateJobObject(m_job.get(), 1);
        }
    }

    void EngineJob::ReadLines(HANDLE pipe, std::function<void(std::string const&)> const& onLine)
    {
        std::string pending;
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        {
            pending.append(buffer, read);
            size_t newline;
            while ((newline = pending.find('\n')) != std::string::npos)
            {
                auto line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                onLine(line);
            }
        }
        if (!pending.empty())
        {
            onLine(pending);
        }
    }

    void EngineJob::ReadStdout()
    {
        ReadLines(m_stdoutRead.get(), [this](std::string const& line) { HandleEventLine(line); });
    }

    void EngineJob::ReadStderr()
    {
        ReadLines(m_stderrRead.get(), [this](std::string const& line)
        {
            if (!line.empty() && m_callbacks.Log)
            {
                m_callbacks.Log(Utf8ToWide(line));
            }
        });
    }

    void EngineJob::HandleEventLine(std::string const& line)
    {
        JsonObject obj{ nullptr };
        if (!JsonObject::TryParse(winrt::to_hstring(line), obj))
        {
            if (m_callbacks.Log && !line.empty())
            {
                m_callbacks.Log(Utf8ToWide(line));
            }
            return;
        }

        auto event = obj.GetNamedString(L"event", L"");
        if (event == L"start" && m_callbacks.Started)
        {
            JobStarted started;
            started.Width = static_cast<int>(NumberOr(obj, L"width", 0));
            started.Height = static_cast<int>(NumberOr(obj, L"height", 0));
            started.OutWidth = static_cast<int>(NumberOr(obj, L"out_width", 0));
            started.OutHeight = static_cast<int>(NumberOr(obj, L"out_height", 0));
            started.FinalWidth = static_cast<int>(NumberOr(obj, L"final_width", started.OutWidth));
            started.FinalHeight = static_cast<int>(NumberOr(obj, L"final_height", started.OutHeight));
            started.Fps = NumberOr(obj, L"fps", 0);
            started.Frames = static_cast<int64_t>(NumberOr(obj, L"frames", 0));
            started.SourceFrames = static_cast<int64_t>(NumberOr(obj, L"source_frames", 0));
            started.Encoder = obj.GetNamedString(L"encoder", L"");
            started.Audio = obj.GetNamedBoolean(L"audio", false);
            started.AudioImproved = obj.GetNamedBoolean(L"audio_improved", false);
            m_callbacks.Started(started);
        }
        else if (event == L"progress" && m_callbacks.Progress)
        {
            JobProgress progress;
            progress.Frame = static_cast<int64_t>(NumberOr(obj, L"frame", 0));
            progress.Total = static_cast<int64_t>(NumberOr(obj, L"total", 0));
            progress.Fps = NumberOr(obj, L"fps", 0);
            if (auto eta = obj.TryLookup(L"eta_secs"); eta && eta.ValueType() == JsonValueType::Number)
            {
                progress.EtaSeconds = eta.GetNumber();
            }
            m_callbacks.Progress(progress);
        }
        else if (event == L"probe" && m_callbacks.Probe)
        {
            JobProbe probe;
            probe.Width = static_cast<int>(NumberOr(obj, L"width", 0));
            probe.Height = static_cast<int>(NumberOr(obj, L"height", 0));
            probe.Fps = NumberOr(obj, L"fps", 0);
            probe.Frames = static_cast<int64_t>(NumberOr(obj, L"frames", 0));
            probe.Duration = NumberOr(obj, L"duration", 0);
            probe.Sar = NumberOr(obj, L"sar", 1.0);
            probe.Audio = obj.GetNamedBoolean(L"audio", false);
            m_callbacks.Probe(probe);
        }
        else if (event == L"preview" && m_callbacks.Preview)
        {
            JobPreview preview;
            preview.Time = NumberOr(obj, L"time", 0);
            preview.Frame = static_cast<int64_t>(NumberOr(obj, L"frame", -1));
            preview.SourcePath = StringOr(obj, L"source");
            preview.OutputPath = StringOr(obj, L"output");
            preview.SourceWidth = static_cast<int>(NumberOr(obj, L"source_width", 0));
            preview.SourceHeight = static_cast<int>(NumberOr(obj, L"source_height", 0));
            preview.OutputWidth = static_cast<int>(NumberOr(obj, L"output_width", 0));
            preview.OutputHeight = static_cast<int>(NumberOr(obj, L"output_height", 0));
            m_callbacks.Preview(preview);
        }
        else if (event == L"done")
        {
            m_reportedOutcome = static_cast<int>(JobOutcome::Completed);
        }
        else if (event == L"cancelled")
        {
            m_reportedOutcome = static_cast<int>(JobOutcome::Cancelled);
        }
        else if (m_callbacks.Other)
        {
            m_callbacks.Other(std::wstring(event), obj);
        }
    }

    void EngineJob::WaitForExit()
    {
        WaitForSingleObject(m_process.get(), INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(m_process.get(), &exitCode);

        // Drain remaining output before reporting, so the last log lines arrive first.
        if (m_stdoutThread.joinable())
        {
            m_stdoutThread.join();
        }
        if (m_stderrThread.joinable())
        {
            m_stderrThread.join();
        }
        {
            std::scoped_lock lock(m_stdinLock);
            m_stdinWrite.reset();
        }

        auto reported = m_reportedOutcome.load();
        auto outcome = reported >= 0 ? static_cast<JobOutcome>(reported)
                                     : (exitCode == 0 ? JobOutcome::Completed : JobOutcome::Failed);
        m_running = false;
        if (m_callbacks.Exited)
        {
            m_callbacks.Exited(outcome, exitCode);
        }
    }

    std::wstring EngineJob::BuildCommandLine(std::filesystem::path const& exe, std::vector<std::wstring> const& arguments)
    {
        std::wstring commandLine;
        AppendQuoted(commandLine, exe.wstring());
        for (auto const& argument : arguments)
        {
            commandLine += L' ';
            AppendQuoted(commandLine, argument);
        }
        return commandLine;
    }
}
