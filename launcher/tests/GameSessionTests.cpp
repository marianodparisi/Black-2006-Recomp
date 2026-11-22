#include "TempFolder.h"
#include "core/Files.h"
#include "core/GameSession.h"
#include "core/TraceLog.h"

#include <doctest/doctest.h>

using namespace black::core;

TEST_CASE("The game environment follows the settings") {
    const ProjectLayout project(std::filesystem::path("/games/black"));
    Settings settings = defaultSettings();
    settings.threads = 6;
    settings.mouseSensitivity = 1.25;
    settings.bindings = {{"FIRE", "Mouse2"}};
    auto env = gameEnvironment(project, settings, PathStyle::Native);
    CHECK(env.at("RECOMP_KELVIN_THREADS") == "6");
    CHECK(env.at("RECOMP_KELVIN_GPU") == "1");
    CHECK(env.at("RECOMP_BLACK_60HZ") == "1");
    CHECK(env.at("RECOMP_BLACK_MOUSE_SENSITIVITY") == "1.25");
    CHECK(env.at("RECOMP_BLACK_BIND_FIRE") == "Mouse2");
    CHECK(env.at("RECOMP_BLACK_BIND_CROUCH") == "LCtrl,C");
    CHECK(env.at("RECOMP_LANGUAGE") == "en-US");
    CHECK_FALSE(env.contains("RECOMP_BLACK_HZ"));
    CHECK_FALSE(env.contains("RECOMP_KELVIN_ASSIST_QUEUE"));
    CHECK_FALSE(env.contains("RECOMP_PAD_SCRIPT"));
    CHECK_FALSE(env.contains("RECOMP_BLACK_SKIP_STARTUP_VIDEOS"));
    CHECK(env.at("RECOMP_BLACK_INFINITE_HEALTH") == "0");

    settings.smoothHz = 120;
    settings.internalHeight = 1440;
    settings.cameraAspect = "21:9";
    settings.skipStartupVideos = true;
    settings.silverWeaponMode = "force-off";
    settings.infiniteHealth = true;
    env = gameEnvironment(project, settings, PathStyle::Native);
    CHECK(env.at("RECOMP_BLACK_HZ") == "120");
    CHECK_FALSE(env.contains("RECOMP_BLACK_60HZ"));
    CHECK(env.at("RECOMP_KELVIN_ASSIST_QUEUE") == "1");
    CHECK(env.at("RECOMP_BLACK_SILVER_WEAPONS") == "0");
    CHECK_FALSE(env.contains("RECOMP_PAD_SCRIPT"));
    CHECK(env.at("RECOMP_BLACK_SKIP_STARTUP_VIDEOS") == "1");
    CHECK(env.at("RECOMP_BLACK_INFINITE_HEALTH") == "1");
    CHECK_FALSE(env.contains("RECOMP_BLACK_SKIP_MOVIES"));
}

TEST_CASE("Movie skipping never injects keyboard or controller presses") {
    const ProjectLayout project(std::filesystem::path("/games/black"));
    for (const bool startup : {false, true}) {
        for (const bool allMovies : {false, true}) {
            Settings settings = defaultSettings();
            settings.skipStartupVideos = startup;
            settings.skipMovies = allMovies;
            const auto env = gameEnvironment(project, settings, PathStyle::Native);
            CHECK(env.contains("RECOMP_BLACK_SKIP_STARTUP_VIDEOS") == startup);
            CHECK(env.contains("RECOMP_BLACK_SKIP_MOVIES") == allMovies);
            CHECK_FALSE(env.contains("RECOMP_PAD_SCRIPT"));
            CHECK_FALSE(env.contains("RECOMP_INPUT_SCRIPT"));
        }
    }
    const auto migrated = gameEnvironment(project, settingsFromJson(R"({"AdvanceMenus":true})"), PathStyle::Native);
    CHECK(migrated.at("RECOMP_BLACK_SKIP_STARTUP_VIDEOS") == "1");
    CHECK_FALSE(migrated.contains("RECOMP_PAD_SCRIPT"));
}

TEST_CASE("Under Wine the game's paths are on drive Z:") {
    const ProjectLayout project(std::filesystem::path("/home/player/black"));
    const auto env = gameEnvironment(project, defaultSettings(), PathStyle::Wine);
#ifndef _WIN32
    CHECK(env.at("RECOMP_VIDEO_INI") == "Z:\\home\\player\\black\\local\\video.ini");
#endif
    CHECK(env.at("RECOMP_CONTROLS_INI").starts_with("Z:"));
    CHECK(env.at("RECOMP_CONTROLS_INI").find('/') == std::string::npos);
    CHECK(env.at("RECOMP_AUDIO_INI").starts_with("Z:"));
    CHECK(env.at("RECOMP_AUDIO_INI").ends_with("\\local\\audio.ini"));
}

TEST_CASE("Playing needs the build and the disc") {
    const black::tests::TempFolder folder;
    const ProjectLayout project(folder.path());
    CHECK_FALSE(checkReadyToPlay(project).ok());
    folder.write("bin/black_xbox_recomp.exe", "MZ");
    CHECK(checkReadyToPlay(project).error().message.find("disc") != std::string::npos);
}

TEST_CASE("A selected HD texture pack needs its folder and never silently falls back") {
    const black::tests::TempFolder folder;
    const ProjectLayout project(folder.path());
    Settings settings;
    CHECK_FALSE(texturePackProblem(project, settings));
    settings.textures = "hd-pack";
    const auto missing = texturePackProblem(project, settings);
    REQUIRE(missing.has_value());
    CHECK(missing->find("texture_pack_hd") != std::string::npos);
    CHECK(missing->find("Original") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(project.root() / "texture_pack_hd"));
    std::filesystem::create_directory(project.root() / "texture_pack_hd");
    CHECK_FALSE(texturePackProblem(project, settings));
    const auto env = gameEnvironment(project, settings, PathStyle::Native);
    CHECK(env.contains("RECOMP_TEXTURE_PACK"));
    settings.textures = "original";
    CHECK_FALSE(gameEnvironment(project, settings, PathStyle::Native).contains("RECOMP_TEXTURE_PACK"));
}

TEST_CASE("A capped trace log keeps the first lines and says how many it dropped") {
    const black::tests::TempFolder folder;
    {
        TraceLog log(folder.path() / "trace.log", 256, "[KTRACE]");
        for (int i = 0; i < 20; ++i) {
            log.write("[KTRACE] line " + std::to_string(i));
        }
        log.complete();
        log.write("after complete");
        CHECK(log.failure().empty());
    }
    const auto bytes = readFile(folder.path() / "trace.log");
    REQUIRE(bytes.ok());
    const std::string text(bytes.value().begin(), bytes.value().end());
    CHECK(text.starts_with("[KTRACE] line 0\n"));
    CHECK(text.find("[KTRACE] TRUNCATED: 12 lines discarded (limit 256 bytes).\n") != std::string::npos);
    CHECK(text.find("after complete") == std::string::npos);
}
