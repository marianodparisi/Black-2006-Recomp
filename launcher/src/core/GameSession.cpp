#include "core/GameSession.h"

#include "core/ChildProcess.h"
#include "core/GameFiles.h"
#include "core/LanguageBanks.h"
#include "core/Sha256.h"
#include "core/Text.h"
#include "core/TraceLog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <random>
#include <span>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// after windows.h, which it depends on
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace black::core {
namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

namespace {

/// Raster sizes from this many pixels (3440x1440 is 4.95 million) get the renderer's queue-assistance profile.
constexpr long long kHeavyRasterPixels = 4000000;

std::string shortest(double value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return {buffer.data(), result.ptr};
}

std::string gamePath(const fs::path& path, PathStyle style) {
    if (style == PathStyle::Native) {
        return toUtf8(fs::absolute(path));
    }
    std::string text = "Z:" + toUtf8(fs::absolute(path));
    std::replace(text.begin(), text.end(), '/', '\\');
    return text;
}

std::string utcStamp() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::array<char, 32> text{};
    const std::size_t length = std::strftime(text.data(), text.size(), "%Y%m%dT%H%M%S", &utc);
    std::array<char, 8> millis{};
    std::snprintf(millis.data(), millis.size(), "%03d", static_cast<int>(ms));
    return std::string(text.data(), length) + millis.data() + "Z";
}

std::string isoNow() {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::array<char, 32> text{};
    return {text.data(), std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%SZ", &utc)};
}

std::string randomHex8() {
    std::random_device device;
    std::array<char, 9> text{};
    std::snprintf(text.data(), text.size(), "%08x", static_cast<unsigned>(device()));
    return text.data();
}

#ifdef _WIN32
/// Upper-case hex SHA-256 of the project folder, naming its lock so two copies of the project do not block each
/// other.
std::string projectIdentity(const ProjectLayout& project) {
    std::string root = toUtf8(fs::absolute(project.root()));
    while (!root.empty() && (root.back() == '/' || root.back() == '\\')) {
        root.pop_back();
    }
    std::transform(root.begin(), root.end(), root.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    Sha256 hash;
    hash.update(std::as_bytes(std::span(root)));
    return Sha256::toHex(hash.finish());
}

/// A copy of this project's game already running, started from anywhere: its process id, or 0.
unsigned long runningGame(const fs::path& executable) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    unsigned long found = 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Process32FirstW(snapshot, &entry); more != 0 && found == 0;
         more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"black_xbox_recomp.exe") != 0) {
            continue;
        }
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process == nullptr) {
            found = entry.th32ProcessID;  // not ours to inspect: assume it is the game
            continue;
        }
        std::array<wchar_t, 32768> name{};
        auto size = static_cast<DWORD>(name.size());
        if (QueryFullProcessImageNameW(process, 0, name.data(), &size) != 0 &&
            _wcsicmp(name.data(), fs::absolute(executable).c_str()) == 0) {
            found = entry.th32ProcessID;
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
}
#endif

/// How the game is started on this host: directly on Windows; elsewhere through Wine, or Proton when BLACK_PROTON
/// names one (the same variables Play BLACK.sh used).
struct LaunchCommand {
    fs::path program;
    std::vector<std::string> arguments;
    std::map<std::string, std::string> environment;
};

Result<LaunchCommand> launchCommand(const ProjectLayout& project, const fs::path& executable) {
#ifdef _WIN32
    (void)project;
    return LaunchCommand{executable, {}, {}};
#else
    const auto variable = [](const char* name) -> std::string {
        const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read before any thread changes it
        return value == nullptr ? std::string() : value;
    };
    LaunchCommand command;
    if (const std::string proton = variable("BLACK_PROTON"); !proton.empty()) {
        if (variable("STEAM_COMPAT_CLIENT_INSTALL_PATH").empty()) {
            return Error{"Set STEAM_COMPAT_CLIENT_INSTALL_PATH to your Steam folder to play through Proton."};
        }
        std::string data = variable("BLACK_COMPAT_DATA_PATH");
        if (data.empty()) {
            data = (project.root() / ".proton").string();
        }
        std::error_code error;
        fs::create_directories(data, error);
        command.program = proton;
        command.arguments = {"run", executable.string()};
        command.environment["STEAM_COMPAT_DATA_PATH"] = data;
        return command;
    }
    std::string prefix = variable("BLACK_WINEPREFIX");
    if (prefix.empty()) {
        prefix = (project.root() / ".wine").string();
    }
    if (!fs::path(prefix).is_absolute()) {
        return Error{"BLACK_WINEPREFIX must be an absolute path."};
    }
    const std::string wine = variable("BLACK_WINE");
    command.program = wine.empty() ? "wine" : wine;
    command.arguments = {executable.string()};
    command.environment["WINEPREFIX"] = prefix;
    return command;
#endif
}

Result<bool> writeText(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        return Error{"Could not write " + toUtf8(path)};
    }
    return true;
}

}  // namespace

struct GameSession::Lock {
#ifdef _WIN32
    HANDLE mutex = nullptr;
    ~Lock() {
        if (mutex != nullptr) {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
    }
#else
    int file = -1;
    ~Lock() {
        if (file >= 0) {
            close(file);
        }
    }
#endif
    Lock() = default;
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;

    [[nodiscard]] static std::unique_ptr<Lock> acquire(const ProjectLayout& project) {
        auto lock = std::make_unique<Lock>();
#ifdef _WIN32
        const std::wstring name = L"Local\\BlackXboxGame-" + pathFromUtf8(projectIdentity(project)).wstring();
        lock->mutex = CreateMutexW(nullptr, FALSE, name.c_str());
        if (lock->mutex == nullptr) {
            return nullptr;
        }
        const DWORD state = WaitForSingleObject(lock->mutex, 0);
        if (state != WAIT_OBJECT_0 && state != WAIT_ABANDONED) {
            return nullptr;
        }
#else
        std::error_code error;
        fs::create_directories(project.localDirectory(), error);
        const std::string path = (project.localDirectory() / ".game.lock").string();
        lock->file =
            open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);  // NOLINT(cppcoreguidelines-pro-type-vararg)
        if (lock->file < 0 || flock(lock->file, LOCK_EX | LOCK_NB) != 0) {
            return nullptr;
        }
#endif
        return lock;
    }
};

/// XC_LANGUAGE for a menu language on the PAL disc, which picks its official bank from it (English otherwise).
const char* palConsoleLanguage(std::string_view language) {
    return language == "de" ? "3" : language == "fr" ? "4" : language == "es" ? "5" : language == "it" ? "6" : "1";
}

std::map<std::string, std::string> gameEnvironment(const ProjectLayout& project, const Settings& settings,
                                                   PathStyle style) {
    std::map<std::string, std::string> env = {
        {"RECOMP_AC97_READY", "1"},
        {"RECOMP_APU_DSP_ACK", "0x83218810"},
        {"RECOMP_NV2A_KELVIN", "1"},
        {"RECOMP_VBLANK", "1"},
        {"RECOMP_VBLANK_KEEP_GRID", "1"},  // vblanks a stalled timer misses are owed, not dropped
        {"RECOMP_USB", "1"},
        {"RECOMP_FB_WINDOW", "1"},
        {"RECOMP_KELVIN_THREADS", std::to_string(settings.threads)},
        {"RECOMP_VIDEO_INI", gamePath(project.localDirectory() / "video.ini", style)},
        {"RECOMP_CONTROLS_INI", gamePath(project.localDirectory() / "controls.ini", style)},
        {"RECOMP_AUDIO_INI", gamePath(project.localDirectory() / "audio.ini", style)},  // SFX/music volumes
        {"RECOMP_LANGUAGE", settings.menuLanguage},
        {"RECOMP_SUBTITLES", settings.subtitles ? "1" : "0"},
        {"RECOMP_BLACK_INPUT_MODE", settings.inputMode},
        {"RECOMP_BLACK_PROMPT_MODE", settings.promptMode},
        {"RECOMP_BLACK_MOUSE_SENSITIVITY", shortest(settings.mouseSensitivity)},
        {"RECOMP_BLACK_MOUSE_INVERT_Y", settings.invertY ? "1" : "0"},
    };
    if (settings.gpuRenderer) {
        env["RECOMP_KELVIN_GPU"] = "1";
        env["RECOMP_KGPU_CACHE"] = gamePath(project.localDirectory() / "kgpu-cache", style);  // compiled shaders
        env["RECOMP_KGPU_PRECOMPILE"] = settings.precompileShaders ? "1" : "0";
    }
    if (inspectGameFiles(project.defaultXbe()).sha256 == kPalXbeSha256) {
        env["RECOMP_XBOX_LANGUAGE"] = palConsoleLanguage(settings.menuLanguage);
    }
    if (settings.smooth60 && settings.smoothHz != 0) {
        env["RECOMP_BLACK_HZ"] = std::to_string(settings.smoothHz);
    } else if (settings.smooth60) {
        env["RECOMP_BLACK_60HZ"] = "1";
    }
    if (settings.visualTraceEnabled) {
        env["RECOMP_KELVIN_TRACE"] = normalizeTraceFrames(settings.visualTraceFrames).value_or("");
    }
    if (!settings.kgpuDrawLogFrames.empty()) {
        env["RECOMP_KGPU_DRAWLOG"] = normalizeDrawLogFrames(settings.kgpuDrawLogFrames).value_or("");
    }
    if (settings.dspTraceEnabled) {
        env["RECOMP_APU_TRACE_DSP_STATE"] = "1";
    }
    if (settings.infiniteAmmo) {
        env["RECOMP_BLACK_INFINITE_AMMO"] = "1";
    }
    if (settings.textures == "hd-pack" && fs::is_directory(project.root() / "texture_pack_hd")) {
        env["RECOMP_TEXTURE_PACK"] = gamePath(project.root() / "texture_pack_hd", style);  // replacement textures
        const fs::path map =
            project.root() / "textures" / "black-hd-reimagined.map";  // which file replaces which texture
        if (fs::is_regular_file(map))
            env["RECOMP_TEXTURE_PACK_MAP"] = gamePath(map, style);
    }
    if (settings.silverWeaponMode == "force-on") {
        env["RECOMP_BLACK_SILVER_WEAPONS"] = "1";
    } else if (settings.silverWeaponMode == "force-off") {
        env["RECOMP_BLACK_SILVER_WEAPONS"] = "0";
    }
    const auto raster = rasterSize(settings);
    if (static_cast<long long>(raster[0]) * raster[1] >= kHeavyRasterPixels) {
        env["RECOMP_KELVIN_ASSIST_QUEUE"] = "1";
        env["RECOMP_KELVIN_SPIN_US"] = "250";
    }
    for (const auto& action : bindings::actions()) {
        env["RECOMP_BLACK_BIND_" + std::string(action.name)] = bindings::effective(settings.bindings, action);
    }
    if (settings.skipMovies) {
        env["RECOMP_BLACK_SKIP_MOVIES"] = "1";
    }
    if (settings.skipStartupVideos) {
        env["RECOMP_BLACK_SKIP_STARTUP_VIDEOS"] = "1";
    }
    return env;
}

Result<fs::path> checkReadyToPlay(const ProjectLayout& project) {
    const auto executable = project.gameExecutable();
    if (!executable) {
        return Error{"The game has not been built yet."};
    }
    const GameFilesStatus files = inspectGameFiles(project.defaultXbe());
    if (files.state != GameFilesState::Supported) {
        return Error{"The game files are missing or not the supported BLACK disc. Set up your disc first."};
    }
    std::error_code error;
    const fs::path game = project.gameDirectory();
    if (!fs::is_regular_file(game / "GlobData.bin", error) || !fs::is_directory(game / "levels", error) ||
        !fs::is_directory(game / "sound", error) || !fs::is_directory(game / "videos", error)) {
        return Error{"Some of the disc's files are missing. Set up your disc again."};
    }
    return *executable;
}

std::optional<std::string> texturePackProblem(const ProjectLayout& project, const Settings& settings) {
    std::error_code error;
    if (settings.textures == "hd-pack" && !fs::is_directory(project.root() / "texture_pack_hd", error)) {
        return "The HD texture pack folder is missing. Add the pack to texture_pack_hd, or choose Original in "
               "Settings > Game > Textures.";
    }
    return std::nullopt;
}

Result<std::unique_ptr<GameSession>> GameSession::start(const ProjectLayout& project, const Settings& settings) {
    const auto executable = checkReadyToPlay(project);
    if (!executable) {
        return executable.error();
    }
    if (const auto problem = settingsProblem(settings)) {
        return Error{*problem};
    }
    if (const auto problem = texturePackProblem(project, settings)) {
        return Error{*problem};
    }
    std::unique_ptr<GameSession> session(new GameSession());
    session->m_lock = Lock::acquire(project);
    if (!session->m_lock) {
        return Error{"BLACK is already running from this folder."};
    }
#ifdef _WIN32
    if (const unsigned long pid = runningGame(executable.value()); pid != 0) {
        return Error{"BLACK is already running (process " + std::to_string(pid) + "). Close it first."};
    }
    constexpr PathStyle style = PathStyle::Native;
#else
    constexpr PathStyle style = PathStyle::Wine;
#endif
    auto command = launchCommand(project, executable.value());
    if (!command) {
        return command.error();
    }
    const std::string xbeSha256 = inspectGameFiles(project.defaultXbe()).sha256;
    if (xbeSha256 != kPalXbeSha256) {
        if (auto bank = installLanguageBank(project, settings.menuLanguage); !bank) {
            return bank.error();
        }
    }

    std::error_code error;
    fs::create_directories(project.root() / "save", error);
    fs::create_directories(project.localDirectory(), error);
    session->m_directory = project.root() / "reports" / ("launcher-" + utcStamp() + "-" + randomHex8());
    if (!fs::create_directories(session->m_directory, error)) {
        return Error{"Could not create " + toUtf8(session->m_directory)};
    }
    const fs::path& folder = session->m_directory;
    session->m_log = std::make_unique<TraceLog>(folder / "session.log");
    if (settings.visualTraceEnabled) {
        session->m_visualTrace =
            std::make_unique<TraceLog>(folder / "visual-trace.log", TraceLog::kDiagnosticLimit, "[KTRACE]");
    }
    if (settings.dspTraceEnabled) {
        session->m_dspTrace =
            std::make_unique<TraceLog>(folder / "audio-dsp-trace.log", TraceLog::kDiagnosticLimit, "[DSPTRACE]");
    }
    if (!settings.kgpuDrawLogFrames.empty()) {
        session->m_drawTrace =
            std::make_unique<TraceLog>(folder / "kgpu-draw-trace.log", TraceLog::kDiagnosticLimit, "[KDL]");
    }

    // The game's own settings pages may have changed these since the launcher saved them: only write missing ones
    const fs::path video = project.localDirectory() / "video.ini";
    const fs::path controls = project.localDirectory() / "controls.ini";
    if (!fs::is_regular_file(video, error)) {
        if (auto written = writeText(video, videoIni(settings)); !written) {
            return written.error();
        }
    }
    if (!fs::is_regular_file(controls, error)) {
        if (auto written = writeText(controls, bindings::toControlsIni({}, settings.bindings)); !written) {
            return written.error();
        }
    }
    fs::copy_file(video, folder / "video.ini", fs::copy_options::overwrite_existing, error);

    std::map<std::string, std::string> environment = gameEnvironment(project, settings, style);
    const auto exeHash = sha256File(executable.value());
    const bool drawLog = !settings.kgpuDrawLogFrames.empty();
    Json launch;
    launch["startedUtc"] = isoNow();
    launch["executable"] = toUtf8(executable.value());
    launch["exeSha256"] = exeHash ? Sha256::toHex(exeHash.value()) : "";
    launch["workingDirectory"] = toUtf8(project.root());
    launch["xbeSha256"] = xbeSha256;
    launch["menuLanguage"] = settings.menuLanguage;
    launch["environment"] = environment;
    launch["visualTraceEnabled"] = settings.visualTraceEnabled;
    launch["visualTraceFrames"] =
        settings.visualTraceEnabled ? Json(normalizeTraceFrames(settings.visualTraceFrames).value_or("")) : Json();
    launch["dspTraceEnabled"] = settings.dspTraceEnabled;
    launch["dspTraceFile"] = settings.dspTraceEnabled ? Json("audio-dsp-trace.log") : Json();
    launch["kgpuDrawLogEnabled"] = drawLog;
    launch["kgpuDrawLogFrames"] =
        drawLog ? Json(normalizeDrawLogFrames(settings.kgpuDrawLogFrames).value_or("")) : Json();
    launch["kgpuDrawLogFile"] = drawLog ? Json("kgpu-draw-trace.log") : Json();
    if (auto written = writeText(folder / "launch.json", launch.dump(2) + "\n"); !written) {
        return written.error();
    }
    session->writeLine(
        "[launcher] BLACK PC / " + std::string(settings.gpuRenderer ? "GPU Kelvin (Direct3D 11)" : "CPU Kelvin") +
            (settings.smooth60 ? " / " + std::to_string(settings.smoothHz != 0 ? settings.smoothHz : 60) + " FPS timing"
                               : "") +
            " / " + std::to_string(settings.threads) + " threads",
        false);
    session->writeLine("[launcher] menu text: " + settings.menuLanguage, false);

    environment.insert(command.value().environment.begin(), command.value().environment.end());
    GameSession* raw = session.get();
    auto process = ChildProcess::start({
        .program = command.value().program,
        .arguments = command.value().arguments,
        .workingDirectory = project.root(),
        .dropPrefixes = {"RECOMP_"},
        .environment = std::move(environment),
        .onLine = [raw](std::string_view line, bool isError) { raw->writeLine(line, isError); },
    });
    if (!process) {
        return process.error();
    }
    session->m_process = std::move(process).value();
    session->m_waiter = std::thread([raw, project] { raw->finish(project); });
    return session;
}

GameSession::~GameSession() {
    if (m_waiter.joinable()) {
        m_waiter.join();
    }
}

void GameSession::writeLine(std::string_view line, bool isError) {
    std::string_view trimmed = line;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
        trimmed.remove_prefix(1);
    }
    const auto route = [&](const std::unique_ptr<TraceLog>& trace) {
        if (trace) {
            trace->write(line);
        }
    };
    if (trimmed.starts_with("[KTRACE]")) {
        route(m_visualTrace);
    } else if (trimmed.starts_with("[DSPTRACE]")) {
        route(m_dspTrace);
    } else if (trimmed.starts_with("[KDL]")) {
        route(m_drawTrace);
    } else if (m_log) {
        m_log->write(isError ? "[stderr] " + std::string(line) : std::string(line));
    }
}

void GameSession::finish(const ProjectLayout& project) {
    const int code = m_process->wait();
    writeLine("[launcher] Exit code: " + std::to_string(code), false);
    std::string failure;
    for (const auto* trace : {&m_log, &m_visualTrace, &m_dspTrace, &m_drawTrace}) {
        if (*trace) {
            (*trace)->complete();
            if (failure.empty()) {
                failure = (*trace)->failure();
            }
        }
    }
    std::error_code error;
    const fs::path kernel = project.root() / "xbox_kernel.log";
    if (fs::is_regular_file(kernel, error)) {
        fs::copy_file(kernel, m_directory / "kernel.log", fs::copy_options::overwrite_existing, error);
    }
    Json exit;
    exit["exitCode"] = code;
    exit["finishedUtc"] = isoNow();
    exit["stopped"] = m_stopRequested.load();
    exit["logFailure"] = failure.empty() ? Json() : Json(failure);
    if (auto written = writeText(m_directory / "exit.json", exit.dump(2) + "\n"); !written && failure.empty()) {
        failure = written.error().message;
    }
    {
        const std::scoped_lock lock(m_mutex);
        m_exitCode = code;
        m_logFailure = failure;
    }
    m_lock.reset();
    m_finished = true;
}

std::optional<int> GameSession::exitCode() const {
    const std::scoped_lock lock(m_mutex);
    return m_exitCode;
}

std::string GameSession::logFailure() const {
    const std::scoped_lock lock(m_mutex);
    return m_logFailure;
}

void GameSession::stop() {
    if (m_process && !finished()) {
        m_stopRequested = true;
        writeLine("[launcher] Stop requested", false);
        m_process->kill();
    }
}

int GameSession::wait() {
    if (m_waiter.joinable()) {
        m_waiter.join();
    }
    return exitCode().value_or(-1);
}

}  // namespace black::core
