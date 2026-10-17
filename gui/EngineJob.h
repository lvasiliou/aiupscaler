#pragma once

namespace AiUpscaler
{
    enum class JobOutcome
    {
        Completed,
        Cancelled,
        Failed,
    };

    struct JobStarted
    {
        int Width{};
        int Height{};
        int OutWidth{};
        int OutHeight{};
        /// Size of the encoded video after --size/--fit (what the output file will be).
        int FinalWidth{};
        int FinalHeight{};
        double Fps{};
        int64_t Frames{};
        /// Frames in the whole source (differs from Frames for previews); 0 if unknown.
        int64_t SourceFrames{};
        std::wstring Encoder;
        bool Audio{};
    };

    struct JobProgress
    {
        int64_t Frame{};
        int64_t Total{};
        double Fps{};
        std::optional<double> EtaSeconds;
    };

    struct JobProbe
    {
        int Width{};
        int Height{};
        double Fps{};
        int64_t Frames{};
        double Duration{};
        /// Pixel aspect ratio (1.0 = square pixels; anamorphic DVDs differ).
        double Sar{ 1.0 };
        bool Audio{};
    };

    /// A source frame and (when available) its upscaled version, saved as images by the engine.
    struct JobPreview
    {
        double Time{};
        int64_t Frame{ -1 }; // -1 for single-frame previews
        std::wstring SourcePath;
        std::wstring OutputPath; // empty for source-only previews
        int SourceWidth{};
        int SourceHeight{};
        int OutputWidth{};
        int OutputHeight{};
    };

    /// Callbacks are invoked on background threads; marshal to the UI thread yourself.
    struct EngineCallbacks
    {
        std::function<void(JobStarted const&)> Started;
        std::function<void(JobProgress const&)> Progress;
        std::function<void(JobProbe const&)> Probe;
        std::function<void(JobPreview const&)> Preview;
        /// Any other event (e.g. `thumbnail`, `capabilities`) with its raw JSON. JsonObject is
        /// agile, so it can be handed to the UI thread as-is.
        std::function<void(std::wstring const& event, winrt::Windows::Data::Json::JsonObject const&)> Other;
        std::function<void(std::wstring const&)> Log;
        std::function<void(JobOutcome, DWORD exitCode)> Exited;
    };

    /// Runs the Rust engine (`ai-upscaler.exe --json`) as a child process.
    /// Progress arrives as JSON lines on stdout and logs on stderr; writing `cancel`
    /// to stdin stops it cleanly. The process lives in a kill-on-close Job object, so the
    /// engine and its ffmpeg children never outlive this object (or a crashed GUI).
    class EngineJob
    {
    public:
        EngineJob(std::filesystem::path enginePath, std::vector<std::wstring> arguments, EngineCallbacks callbacks);
        ~EngineJob();

        EngineJob(EngineJob const&) = delete;
        EngineJob& operator=(EngineJob const&) = delete;

        void Start();
        void Cancel();
        /// Kills the engine and its ffmpeg children immediately (for jobs that ignore `cancel`).
        void Terminate();
        bool IsRunning() const noexcept { return m_running; }

    private:
        void ReadStdout();
        void ReadStderr();
        void WaitForExit();
        void HandleEventLine(std::string const& line);

        static void ReadLines(HANDLE pipe, std::function<void(std::string const&)> const& onLine);
        static std::wstring BuildCommandLine(std::filesystem::path const& exe, std::vector<std::wstring> const& arguments);

        std::filesystem::path m_enginePath;
        std::vector<std::wstring> m_arguments;
        EngineCallbacks m_callbacks;

        wil::unique_handle m_job;
        wil::unique_handle m_process;
        wil::unique_handle m_stdinWrite;
        wil::unique_handle m_stdoutRead;
        wil::unique_handle m_stderrRead;

        std::thread m_stdoutThread;
        std::thread m_stderrThread;
        std::thread m_waitThread;

        std::atomic<bool> m_running{ false };
        std::atomic<int> m_reportedOutcome{ -1 };
        std::mutex m_stdinLock;
    };
}
