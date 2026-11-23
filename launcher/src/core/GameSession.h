#pragma once

#include "core/ProjectLayout.h"
#include "core/Result.h"
#include "core/Settings.h"

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace black::core {

class ChildProcess;
class ProjectLock;
class TraceLog;

/// How the game sees a host path: as is on Windows; under Wine, the Unix path on drive Z:.
enum class PathStyle { Native, Wine };

/// The RECOMP_* variables the game reads. Video and controls settings are not among them: the game reads
/// local/video.ini and local/controls.ini, which its own settings pages also edit.
[[nodiscard]] std::map<std::string, std::string> gameEnvironment(const ProjectLayout& project, const Settings& settings,
                                                                 PathStyle style, bool fullscreen = false);

/// The game executable, once the build and the disc files are both in place.
[[nodiscard]] Result<std::filesystem::path> checkReadyToPlay(const ProjectLayout& project);
/// A selected HD pack must have its own folder; choosing Original never requires a pack.
[[nodiscard]] std::optional<std::string> texturePackProblem(const ProjectLayout& project, const Settings& settings);

/// One run of the game. Its output goes to reports/launcher-<time>-<id>/session.log, diagnostics to their own
/// capped logs beside it, then the kernel log and exit.json when it ends.
class GameSession {
public:
    [[nodiscard]] static Result<std::unique_ptr<GameSession>> start(const ProjectLayout& project,
                                                                    const Settings& settings, bool fullscreen = false);

    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;
    GameSession(GameSession&&) = delete;
    GameSession& operator=(GameSession&&) = delete;
    /// Waits for the game to end (it is not stopped).
    ~GameSession();

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return m_directory; }
    [[nodiscard]] bool finished() const noexcept { return m_finished.load(); }
    /// Set once finished().
    [[nodiscard]] std::optional<int> exitCode() const;
    /// Why a log could not be written, if one could not.
    [[nodiscard]] std::string logFailure() const;
    [[nodiscard]] bool stopRequested() const noexcept { return m_stopRequested.load(); }

    void stop();
    int wait();

private:
    GameSession() = default;
    void writeLine(std::string_view line, bool isError);
    void finish(const ProjectLayout& project);

    std::filesystem::path m_directory;
    std::unique_ptr<ProjectLock> m_lock;
    std::unique_ptr<TraceLog> m_log;
    std::unique_ptr<TraceLog> m_visualTrace;
    std::unique_ptr<TraceLog> m_dspTrace;
    std::unique_ptr<TraceLog> m_drawTrace;
    std::unique_ptr<ChildProcess> m_process;
    std::thread m_waiter;
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_stopRequested{false};
    std::optional<int> m_exitCode;
    std::string m_logFailure;
    mutable std::mutex m_mutex;
};

}  // namespace black::core
