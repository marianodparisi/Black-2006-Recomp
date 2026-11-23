#include "core/GameBuild.h"

#include "core/ChildProcess.h"
#include "core/LanguageBanks.h"
#include "core/Text.h"
#include "core/TraceLog.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <regex>

namespace black::core {
namespace fs = std::filesystem;
namespace {

constexpr std::size_t kLastLines = 40;

bool isFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

std::string fileStamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::array<char, 32> text{};
    return {text.data(), std::strftime(text.data(), text.size(), "%Y%m%d-%H%M%S", &local)};
}

#ifdef _WIN32
std::size_t sourceFiles(const fs::path& folder) {
    std::size_t count = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        const std::string extension = toUtf8(it->path().extension());
        if (extension == ".c" || extension == ".cpp" || extension == ".cc") {
            ++count;
        }
    }
    return count;
}
#endif

/// Runs the build's programs one after another, with their output logged and turned into progress.
class Runner {
public:
    Runner(TraceLog& log, std::stop_token stop, const std::function<void(const BuildProgress&)>& report,
           std::map<std::string, std::string> environment)
        : m_log(log), m_stop(std::move(stop)), m_report(report), m_environment(std::move(environment)) {}

    void enter(BuildStage stage, std::optional<float> fraction = std::nullopt) {
        const std::scoped_lock lock(m_mutex);
        m_progress.stage = stage;
        m_progress.fraction = fraction;
        m_progress.activity.clear();
        m_log.write("== " + std::to_string(static_cast<int>(stage) + 1) + "/" + std::to_string(kBuildStages));
        m_report(m_progress);
    }

    void skip(BuildStage stage) {
        const std::scoped_lock lock(m_mutex);
        m_progress.skipped.at(static_cast<std::size_t>(stage)) = true;
        m_report(m_progress);
    }

    /// Reads each output line (under the lock) to move the progress on.
    void parse(std::function<void(std::string_view, BuildProgress&)> parser) { m_parser = std::move(parser); }

    /// The program's exit code, or -1 when it could not start.
    int run(const fs::path& program, const std::vector<std::string>& arguments, const fs::path& directory) {
        if (m_stop.stop_requested()) {
            return -1;
        }
        std::string command = "> " + toUtf8(program);
        for (const std::string& argument : arguments) {
            command += " " + argument;
        }
        m_log.write(command);
        ChildOptions options;
        options.program = program;
        options.arguments = arguments;
        options.workingDirectory = directory;
        options.environment = m_environment;
        options.killTree = true;
        options.onLine = [this](std::string_view line, bool) {
            const std::scoped_lock lock(m_mutex);
            m_log.write(line);
            m_last.emplace_back(line);
            if (m_last.size() > kLastLines) {
                m_last.pop_front();
            }
            if (!line.empty()) {
                m_progress.activity = std::string(line);
            }
            if (m_parser) {
                m_parser(line, m_progress);
            }
            m_report(m_progress);
        };
        auto child = ChildProcess::start(std::move(options));
        if (!child) {
            note(child.error().message);
            return -1;
        }
        const std::stop_callback stopping(m_stop, [&child] { child.value()->kill(); });
        const int code = child.value()->wait();
        m_log.write("< exit code " + std::to_string(code));
        return code;
    }

    void note(const std::string& line) {
        const std::scoped_lock lock(m_mutex);
        m_log.write(line);
        m_last.push_back(line);
    }

    [[nodiscard]] std::vector<std::string> lastLines() const {
        const std::scoped_lock lock(m_mutex);
        return {m_last.begin(), m_last.end()};
    }

    [[nodiscard]] BuildStage stage() const {
        const std::scoped_lock lock(m_mutex);
        return m_progress.stage;
    }

private:
    TraceLog& m_log;
    std::stop_token m_stop;
    const std::function<void(const BuildProgress&)>& m_report;
    std::map<std::string, std::string> m_environment;
    std::function<void(std::string_view, BuildProgress&)> m_parser;
    BuildProgress m_progress;
    std::deque<std::string> m_last;
    mutable std::mutex m_mutex;
};

/// The menu text in every language, from the player's retail English bank. A problem here leaves the game built.
std::string buildLanguages(const ProjectLayout& project, Runner& runner, const fs::path& python) {
    const auto english = preserveEnglishBank(project);
    if (!english) {
        return english.error().message;
    }
    const fs::path root = project.root();
    const fs::path scripts = root / "scripts" / "localization";
    const fs::path source = project.localDirectory() / "language-banks" / "en-US" / "MainUS.bin";
    const auto bank = [&](const char* language) {
        return toUtf8(project.localDirectory() / "language-banks" / language / "MainUS.bin");
    };
    std::string failed;
    if (runner.run(python,
                   {toUtf8(scripts / "build_pt_br_bank.py"), toUtf8(source),
                    toUtf8(root / "translations" / "pt-BR" / "MainUS.json"), bank("pt-BR")},
                   root) != 0) {
        failed += " pt-BR";
    }
    for (const char* language : {"es", "fr", "de", "it"}) {
        const fs::path catalog = root / "localization" / "game" / (std::string(language) + ".jsonl");
        if (runner.run(python, {toUtf8(scripts / "build_mainus.py"), toUtf8(source), toUtf8(catalog), bank(language)},
                       root) != 0) {
            failed += std::string(" ") + language;
        }
    }
    return failed.empty() ? std::string() : "These menu languages could not be made:" + failed;
}

}  // namespace

std::optional<float> generateFraction(std::string_view line) {
    if (line.starts_with("Generated ")) {
        return 1.0f;
    }
    constexpr std::array<std::pair<std::string_view, float>, 6> kSteps = {{
        {"Running tools.xbe_parser", 0.02f},
        {"Running tools.disasm", 0.08f},
        {"Running tools.recomp.icall_feedback", 0.25f},
        {"Running tools.func_id", 0.32f},
        {"Running tools.abi_analysis", 0.42f},
        {"Running tools.recomp", 0.55f},
    }};
    for (const auto& [prefix, fraction] : kSteps) {
        if (line == prefix || (line.starts_with(prefix) && line.size() > prefix.size() && line[prefix.size()] == ' ')) {
            return fraction;
        }
    }
    return std::nullopt;
}

bool compiledFileLine(std::string_view line) {
    static const std::regex pattern(R"(^\s*[\w.+-]+\.(c|cc|cpp|cxx)\s*$)");
    return std::regex_match(line.begin(), line.end(), pattern);
}

std::optional<float> ninjaFraction(std::string_view line) {
    static const std::regex pattern(R"(^\[(\d+)/(\d+)\])");
    std::match_results<std::string_view::const_iterator> match;
    if (!std::regex_search(line.begin(), line.end(), match, pattern)) {
        return std::nullopt;
    }
    const float done = std::stof(match[1].str());
    const float total = std::stof(match[2].str());
    return total > 0 ? done / total : 0.0f;
}

bool generatedCodeIsCurrent(const ProjectLayout& project) {
    const fs::path root = project.root();
    const fs::path audit = root / "src" / "recomp" / "gen" / "post_transform_audit.json";
    std::error_code error;
    const auto made = fs::last_write_time(audit, error);
    if (error) {
        return false;
    }
    for (const fs::path& input :
         {project.defaultXbe(), root / "scripts" / "generate.py", root / "src" / "recomp_manual.c",
          root / "scripts" / "icall_seed_functions.json", project.localDirectory() / "icall_targets.json",
          project.localDirectory() / "icall_targets.dump"}) {
        std::error_code missing;
        const auto changed = fs::last_write_time(input, missing);
        if (!missing && changed > made) {
            return false;
        }
    }
    // The recompiler decides what the generated code says too.
    const fs::path recompiler = root / "third_party" / "xboxrecomp" / "tools" / "recomp";
    for (fs::recursive_directory_iterator it(recompiler, error), end; !error && it != end; it.increment(error)) {
        const fs::path& path = it->path();
        if (path.extension() != ".py" || toUtf8(path.filename()).starts_with("test_")) {
            continue;
        }
        std::error_code missing;
        const auto changed = fs::last_write_time(path, missing);
        if (!missing && changed > made) {
            return false;
        }
    }
    return true;
}

BuildOutcome buildGame(const ProjectLayout& project, const Toolchain& toolchain, const BuildOptions& options,
                       std::stop_token stop, const std::function<void(const BuildProgress&)>& report) {
    const fs::path root = project.root();
    BuildOutcome outcome;
    std::error_code error;
    fs::create_directories(root / "reports", error);
    outcome.log = root / "reports" / ("build-" + fileStamp() + ".log");
    TraceLog log(outcome.log);
    Runner runner(log, stop, report,
                  {{"PATH", toolchain.pathVariable()}, {"PYTHONUTF8", "1"}, {"CARGO_TERM_COLOR", "never"}});

    const auto fail = [&](std::string message) {
        outcome.stage = runner.stage();
        outcome.stopped = stop.stop_requested();
        outcome.message = outcome.stopped ? std::string("Stopped.") : std::move(message);
        runner.note(outcome.message);
        outcome.lastLines = runner.lastLines();
        return outcome;
    };
    if (!toolchain.ready()) {
        return fail("Some of the tools the build needs are missing.");
    }
    const Tool* python = toolchain.find("python");

    runner.enter(BuildStage::Prepare);
    if (fs::exists(root / ".git", error) &&
        (!isFile(root / "third_party" / "xboxrecomp" / "CMakeLists.txt") ||
         !isFile(root / "third_party" / "dsp56300" / "crates" / "emu-ffi" / "Cargo.toml"))) {
        const Tool* git = toolchain.find("git");
        if (git == nullptr || !git->found ||
            runner.run(git->program, {"-C", toUtf8(root), "submodule", "update", "--init", "--recursive"}, root) != 0) {
            return fail("Could not fetch the project's missing parts (git submodule update).");
        }
    }
    const bool regenerate = options.fromScratch || !generatedCodeIsCurrent(project);

#ifdef _WIN32
    const fs::path venvPython = root / ".venv" / "Scripts" / "python.exe";
    if (!isFile(venvPython) && runner.run(python->program, {"-m", "venv", toUtf8(root / ".venv")}, root) != 0) {
        return fail("Could not set up Python for the build (.venv).");
    }
    if (runner.run(venvPython,
                   {"-m", "pip", "install", "--no-cache-dir", "--disable-pip-version-check", "capstone==5.0.7"},
                   root) != 0) {
        return fail("Could not install the disassembler (capstone) for the build.");
    }

    if (regenerate) {
        runner.enter(BuildStage::Generate, 0.0f);
        runner.parse([](std::string_view line, BuildProgress& progress) {
            if (const auto fraction = generateFraction(line)) {
                progress.fraction = fraction;
            }
        });
        if (runner.run(venvPython, {toUtf8(root / "scripts" / "generate.py")}, root) != 0) {
            return fail("Translating the game's code from your disc failed.");
        }
    } else {
        runner.skip(BuildStage::Generate);
    }

    runner.enter(BuildStage::Configure);
    runner.parse({});
    const fs::path build = root / "build-vs2022";
    const Tool* cmake = toolchain.find("cmake");
    const Tool* rust = toolchain.find("rust");
    if (runner.run(cmake->program,
                   {"-S", toUtf8(root), "-B", toUtf8(build), "-G", "Visual Studio 17 2022", "-A", "x64",
                    "-DCARGO_EXECUTABLE=" + toUtf8(rust->program)},
                   root) != 0) {
        return fail("Setting up the build (CMake) failed.");
    }

    const auto total = static_cast<float>(
        std::max<std::size_t>(1, sourceFiles(root / "src") + sourceFiles(root / "third_party" / "xboxrecomp" / "src")));
    runner.enter(BuildStage::Compile, 0.0f);
    runner.parse([total](std::string_view line, BuildProgress& progress) {
        if (compiledFileLine(line)) {
            ++progress.compiled;
            progress.fraction = std::min(0.99f, static_cast<float>(progress.compiled) / total);
        }
    });
    if (runner.run(cmake->program,
                   {"--build", toUtf8(build), "--config", "Release", "--target", "black_xbox_recomp", "--parallel",
                    "--", "/nodeReuse:false", "/verbosity:minimal"},
                   root) != 0) {
        return fail("Compiling the game failed.");
    }
    const fs::path languagesPython = venvPython;
#else
    // The Linux build (scripts/setup-linux.sh) prepares its pinned cross toolchain, generates and compiles
    std::vector<std::string> arguments = {toUtf8(root / "scripts" / "setup-linux.sh"), "--game-only"};
    if (!regenerate) {
        arguments.emplace_back("--skip-generate");
        runner.skip(BuildStage::Generate);
    }
    runner.parse([](std::string_view line, BuildProgress& progress) {
        if (const auto fraction = generateFraction(line)) {
            progress.stage = BuildStage::Generate;
            progress.fraction = fraction;
        } else if (line.starts_with("-- Configuring") || line.starts_with("-- The C compiler")) {
            progress.stage = BuildStage::Configure;
            progress.fraction.reset();
        } else if (const auto compiled = ninjaFraction(line)) {
            progress.stage = BuildStage::Compile;
            progress.fraction = compiled;
        }
    });
    if (runner.run("bash", arguments, root) != 0) {
        return fail("Building the game failed.");
    }
    const fs::path languagesPython = python->program;
#endif

    runner.enter(BuildStage::Languages);
    runner.parse({});
    outcome.warning = buildLanguages(project, runner, languagesPython);
    if (stop.stop_requested()) {
        return fail("Stopped.");
    }
    outcome.ok = true;
    outcome.stage = BuildStage::Languages;
    outcome.lastLines = runner.lastLines();
    runner.note(outcome.warning.empty() ? "The game is built." : "The game is built. " + outcome.warning);
    return outcome;
}

}  // namespace black::core
