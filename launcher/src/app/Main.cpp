#include "app/EmbeddedAssets.h"
#include "app/LauncherLayer.h"
#include "app/NativeDialogs.h"
#include "app/Theme.h"
#include "app/Translations.h"
#include "core/DiscImport.h"
#include "core/GameBuild.h"
#include "core/GameSession.h"
#include "core/Text.h"

#include "Walnut/Application.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// after windows.h
#include <shellapi.h>
#endif

// Walnut's Application clears this when closed; its EntryPoint.h would define it, but the launcher has its own main()
bool g_ApplicationRunning = true;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

namespace {

Walnut::FontSpecification embeddedFont(const char* name, std::span<const unsigned char> data, float sizePixels) {
    Walnut::FontSpecification font;
    font.Name = name;
    font.Data = data.data();
    font.DataSize = data.size();
    font.SizePixels = sizePixels;
    return font;
}

/// The arguments as UTF-8 (argv is in the ANSI code page on Windows; the wide command line keeps any path intact).
std::vector<std::string> arguments(int argc, char** argv) {
    std::vector<std::string> result;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 0; wide != nullptr && i < count; ++i) {
        result.push_back(black::core::toUtf8(std::filesystem::path(wide[i])));  // NOLINT
    }
    LocalFree(wide);
#else
    for (int i = 0; i < argc; ++i) {
        result.emplace_back(argv[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
#endif
    return result;
}

std::filesystem::path searchStart() {
    // argv[0] can be just "BLACK Launcher" when resolved through PATH.
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(size);
    const std::filesystem::path executable(buffer);
#else
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
#endif
    if (!executable.empty() && black::core::ProjectLayout::locate(executable.parent_path())) {
        return executable.parent_path();
    }
    return std::filesystem::current_path();
}

/// --play: starts the game with the saved settings and no launcher window, and waits for it (the logs need the
/// launcher until the game ends). Returns the game's exit code.
int playWithoutWindow(const black::core::ProjectLayout& project, bool fullscreen, bool dialogs) {
    const auto report = [dialogs](const std::string& message) {
        if (dialogs) {
            black::app::showError("BLACK", message);
        } else {
            std::fprintf(stderr, "BLACK: %s\n", message.c_str());
        }
    };
    const black::core::Settings settings = black::core::loadSettings(project);
    black::app::setLanguage(settings.menuLanguage);
    auto session = black::core::GameSession::start(project, settings, fullscreen);
    if (!session) {
        report(session.error().message);
        return 1;
    }
    const int code = session.value()->wait();
    if (const std::string failure = session.value()->logFailure(); !failure.empty()) {
        report(black::app::trf("The game's log could not be written: {0}", {failure}));
    }
    return code;
}

void say(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

/// --build [--from-scratch]: builds the game in the terminal, as the launcher's Build page does (the setup scripts
/// use it). Missing tools are listed, not installed. Returns 0 once the game is built.
int buildWithoutWindow(const black::core::ProjectLayout& project, bool fromScratch) {
    const black::core::Toolchain toolchain = black::core::detectToolchain(project);
    for (const black::core::Tool& tool : toolchain.tools) {
        say((tool.found      ? "  found    "
             : tool.required ? "  MISSING  "
                             : "  -        ") +
            tool.name + (tool.version.empty() ? std::string() : "  " + tool.version));
    }
    if (!toolchain.ready()) {
        say("Install the missing tools (BLACK Launcher > Build the game does it with winget), then try again.");
        return 1;
    }
    std::string printed;
    auto stage = static_cast<black::core::BuildStage>(-1);
    const auto outcome = black::core::buildGame(project, toolchain, black::core::BuildOptions{fromScratch}, {},
                                                [&](const black::core::BuildProgress& progress) {
                                                    if (progress.stage != stage) {
                                                        stage = progress.stage;
                                                        say("== Stage " + std::to_string(static_cast<int>(stage) + 1) +
                                                            " of " + std::to_string(black::core::kBuildStages));
                                                    }
                                                    if (progress.activity != printed) {
                                                        printed = progress.activity;
                                                        say(printed);
                                                    }
                                                });
    say(outcome.ok ? "The game is built." : outcome.message);
    if (!outcome.warning.empty()) {
        say(outcome.warning);
    }
    say("Log: " + black::core::toUtf8(outcome.log));
    return outcome.ok ? 0 : 1;
}

Walnut::Application* createApplication(const std::filesystem::path& start, std::optional<std::filesystem::path> disc) {
    using black::app::LauncherLayer;

    Walnut::ApplicationSpecification specification;
    specification.Name = "BLACK";
    specification.Width = 960;
    specification.Height = 640;
    specification.WindowResizeable = false;
    specification.CenterWindow = true;
    specification.CustomTitlebar = true;  // Windows; elsewhere Walnut keeps the system frame
    specification.TitlebarShowsName = false;
    specification.TitlebarShowsIcon = false;
    specification.TitlebarColor = black::app::toU32(black::app::colors::kTitlebar);
    specification.UseDockspace = true;  // hosts the custom titlebar; the launcher draws one window below it
    specification.UseViewports = false;
    specification.Fonts = {
        embeddedFont(LauncherLayer::kRowFont, black::assets::rowFont(), 26.0f),
        embeddedFont(LauncherLayer::kTitleFont, black::assets::titleFont(), 44.0f),
    };

    auto* application = new Walnut::Application(specification);  // NOLINT(cppcoreguidelines-owning-memory)
    application->PushLayer(std::make_shared<LauncherLayer>(start, std::move(disc)));
    return application;
}

constexpr const char* kUsage = R"(BLACK Launcher [--project-dir DIRECTORY] [COMMAND]
  (no command) or DISC              Open the launcher, optionally with an ISO or disc folder.
  --import DISC                     Check and import an Xbox ISO/XISO or extracted folder.
  --build [--from-scratch]          Build the game in this portable project.
  --settings                        Print the current launcher settings as JSON.
  --set NAME=VALUE [NAME=VALUE ...] Save settings. Use names from --settings.
  --play [--fullscreen] [--no-dialogs]
                                    Play and return the game's exit code. Fullscreen applies
                                    to this run. Use --no-dialogs for unattended launches.
  --help                            Print this help.)";

int fail(const std::string& message) {
    std::fprintf(stderr, "BLACK: %s\n", message.c_str());
    return 1;
}

int importWithoutWindow(const black::core::ProjectLayout& project, const std::string& path) {
    const auto source = black::core::inspectDiscSource(black::core::pathFromUtf8(path));
    if (!source) {
        return fail(source.error().message);
    }
    if (source.value().release.state != black::core::GameFilesState::Supported) {
        return fail("This disc does not contain the supported Xbox release of BLACK.");
    }
    const auto result = black::core::importDisc(source.value(), project.gameDirectory(), {}, {});
    if (!result) {
        return fail(result.error().message);
    }
    say("Disc imported.");
    return 0;
}

int runLauncher(std::span<const std::string> args) {
    using namespace black::core;
    std::filesystem::path start;
    if (!args.empty() && args.front() == "--project-dir") {
        if (args.size() < 2 || args[1].empty() || !ProjectLayout::isProjectRoot(pathFromUtf8(args[1]))) {
            return fail("--project-dir needs the directory containing the extracted BLACK starter or source tree.");
        }
        start = pathFromUtf8(args[1]);
        args = args.subspan(2);
    } else {
        start = searchStart();
    }
    if (args.size() == 1 && args.front() == "--help") {
        say(kUsage);
        return 0;
    }
    if (args.empty() || (args.size() == 1 && !args.front().starts_with("-"))) {
        const std::unique_ptr<Walnut::Application> application(
            createApplication(start, args.empty() ? std::nullopt : std::optional(pathFromUtf8(args.front()))));
        application->Run();
        return 0;
    }
    const auto project = ProjectLayout::locate(start);
    if (!project) {
        return fail("Cannot find the BLACK project. Use --project-dir DIRECTORY.");
    }
    const auto& command = args.front();
    const auto options = args.subspan(1);
    if (command == "--play") {
        bool fullscreen = false;
        bool dialogs = true;
        for (const auto& option : options) {
            if (option == "--fullscreen") {
                fullscreen = true;
            } else if (option == "--no-dialogs") {
                dialogs = false;
            } else {
                return fail("Unknown play option: " + option);
            }
        }
        return playWithoutWindow(*project, fullscreen, dialogs);
    }
    if (command == "--build" && (options.empty() || (options.size() == 1 && options.front() == "--from-scratch"))) {
        return buildWithoutWindow(*project, !options.empty());
    }
    if (command == "--import" && options.size() == 1 && !options.front().empty()) {
        return importWithoutWindow(*project, options.front());
    }
    if (command == "--settings" && options.empty()) {
        say(settingsJson(loadSettings(*project)));
        return 0;
    }
    if (command == "--set" && !options.empty()) {
        const auto settings = withSettingAssignments(loadSettings(*project), options);
        if (!settings) {
            return fail(settings.error().message);
        }
        const auto saved = saveSettings(*project, settings.value());
        return saved ? 0 : fail(saved.error().message);
    }
    return fail(kUsage);
}

}  // namespace

int main(int argc, char** argv) {
    const auto args = arguments(argc, argv);
    return runLauncher(std::span(args).subspan(args.empty() ? 0 : 1));
}
