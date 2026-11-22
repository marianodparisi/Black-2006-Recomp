#include "TempFolder.h"
#include "core/Files.h"
#include "core/Settings.h"

#include <doctest/doctest.h>

using namespace black::core;

namespace {

std::string fileText(const std::filesystem::path& path) {
    const auto bytes = readFile(path);
    REQUIRE(bytes.ok());
    return {reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()};  // NOLINT
}

}  // namespace

TEST_CASE("Startup video skipping migrates menu automation and respects explicit choices") {
    CHECK_FALSE(defaultSettings().skipStartupVideos);
    CHECK_FALSE(settingsFromJson("{}").skipStartupVideos);
    CHECK(settingsFromJson(R"({"AdvanceMenus":true})").skipStartupVideos);
    CHECK_FALSE(settingsFromJson(R"({"AdvanceMenus":false})").skipStartupVideos);
    CHECK_FALSE(settingsFromJson(R"({"AdvanceMenus":"true"})").skipStartupVideos);
    CHECK_FALSE(settingsFromJson(R"({"AdvanceMenus":true,"SkipStartupVideos":false})").skipStartupVideos);
    CHECK(settingsFromJson(R"({"AdvanceMenus":false,"SkipStartupVideos":true})").skipStartupVideos);
    CHECK_FALSE(settingsFromJson(R"({"AdvanceMenus":true,"SkipStartupVideos":"true"})").skipStartupVideos);
    for (const bool enabled : {false, true}) {
        Settings settings = defaultSettings();
        settings.skipStartupVideos = enabled;
        const auto json = settingsJson(settings);
        CHECK(json.find("AdvanceMenus") == std::string::npos);
        CHECK(settingsFromJson(json).skipStartupVideos == enabled);
        CHECK_FALSE(settingsFromJson(json).skipMovies);
    }
}

TEST_CASE("Infinite health defaults off and preserves only explicit boolean preferences") {
    CHECK_FALSE(defaultSettings().infiniteHealth);
    CHECK_FALSE(settingsFromJson("{}").infiniteHealth);
    CHECK_FALSE(settingsFromJson(R"({"InfiniteHealth":"true"})").infiniteHealth);
    CHECK_FALSE(settingsFromJson(R"({"InfiniteHealth":1})").infiniteHealth);
    for (const bool enabled : {false, true}) {
        Settings settings;
        settings.infiniteHealth = enabled;
        CHECK(settingsFromJson(settingsJson(settings)).infiniteHealth == enabled);
        CHECK_FALSE(settingsFromJson(settingsJson(settings)).infiniteAmmo);
    }
}

TEST_CASE("Gameplay additions default off and preserve explicit independent choices") {
    const Settings defaults = defaultSettings();
    CHECK(defaults.crosshair == "original");
    CHECK(defaults.hitmarkers == "off");
    CHECK(defaults.optional_secondaries == "off");
    const Settings legacy = settingsFromJson(R"({"Schema":3,"MenuLanguage":"en-US"})");
    CHECK(legacy.crosshair == "original");
    CHECK(legacy.hitmarkers == "off");
    CHECK(legacy.optional_secondaries == "off");
    const Settings invalid = settingsFromJson(R"({"Crosshair":"invalid","HitMarkers":"invalid"})");
    CHECK(invalid.crosshair == "original");
    CHECK(invalid.hitmarkers == "off");
    for (const std::string_view crosshair : choices::kCrosshairs) {
        for (const std::string_view markers : choices::kOnOff) {
            Settings wanted = defaults;
            wanted.crosshair = crosshair;
            wanted.hitmarkers = markers;
            const Settings jsonRead = settingsFromJson(settingsJson(wanted));
            CHECK(jsonRead.crosshair == crosshair);
            CHECK(jsonRead.hitmarkers == markers);
            Settings iniRead = defaults;
            CHECK(applyVideoIni(iniRead, videoIni(wanted)) > 0);
            CHECK(iniRead.crosshair == crosshair);
            CHECK(iniRead.hitmarkers == markers);
        }
    }
}

TEST_CASE("Optional secondaries validate, round-trip and preserve legacy settings") {
    for (const std::string_view bad :
         {R"({"OptionalSecondaries":"invalid"})", R"({"OptionalSecondaries":true})", R"({"OptionalSecondaries":1})"}) {
        CHECK(settingsFromJson(bad).optional_secondaries == "off");
    }
    Settings legacy;
    CHECK(applyVideoIni(legacy, "[video]\nvsync=0\n") == 1);
    CHECK(legacy.optional_secondaries == "off");
    for (const std::string_view choice : choices::kOnOff) {
        Settings wanted = defaultSettings();
        wanted.optional_secondaries = choice;
        CHECK(settingsFromJson(settingsJson(wanted)).optional_secondaries == choice);
        Settings ini;
        CHECK(applyVideoIni(ini, videoIni(wanted)) > 0);
        CHECK(ini.optional_secondaries == choice);
        CHECK(applyVideoIni(ini, "optional_secondaries=invalid\noptional_secondaries=1\n") == 0);
        CHECK(ini.optional_secondaries == choice);
    }
    CHECK(applyVideoIni(legacy, "OPTIONAL_SECONDARIES = On\n") == 1);
    CHECK(legacy.optional_secondaries == "on");
}

TEST_CASE("Render size follows the camera aspect, and SSAA doubles it") {
    Settings settings;
    settings.internalHeight = 720;
    CHECK(renderSize(settings) == std::array{960, 720});
    settings.cameraAspect = "16:9";
    CHECK(renderSize(settings) == std::array{1280, 720});
    settings.cameraAspect = "21:9";
    CHECK(renderSize(settings) == std::array{1680, 720});
    settings.antiAliasing = "ssaa";
    CHECK(rasterSize(settings) == std::array{3360, 1440});
    settings.internalHeight = 2160;
    settings.cameraAspect = "32:9";
    CHECK(rasterSize(settings) == renderSize(settings));
}

TEST_CASE("Trace frame ranges are normalized or rejected") {
    CHECK(normalizeTraceFrames(" 120 ") == "120");
    CHECK(normalizeTraceFrames("120-180") == "120-180");
    CHECK(normalizeTraceFrames("0120-0240/030") == "120-240/30");
    CHECK_FALSE(normalizeTraceFrames("120/30"));
    CHECK_FALSE(normalizeTraceFrames("240-120"));
    CHECK_FALSE(normalizeTraceFrames("0"));
    CHECK_FALSE(normalizeTraceFrames("a-b"));
    CHECK(normalizeDrawLogFrames("500") == "500");
    CHECK(normalizeDrawLogFrames("500,120") == "500,120");
    CHECK_FALSE(normalizeDrawLogFrames("500,121"));
    CHECK_FALSE(normalizeDrawLogFrames(""));
}

TEST_CASE("Diagnostics need the matching renderer") {
    Settings settings;
    CHECK_FALSE(settingsProblem(settings));
    settings.visualTraceEnabled = true;
    CHECK(settingsProblem(settings));
    settings.gpuRenderer = false;
    CHECK_FALSE(settingsProblem(settings));
    settings.kgpuDrawLogFrames = "100";
    CHECK(settingsProblem(settings));
}

TEST_CASE("video.ini round-trips and skips what the game would skip") {
    Settings settings;
    settings.internalHeight = 1440;
    settings.verticalFov = 87.5;
    settings.reflections = "ultra";
    settings.rayTracing = "on";
    settings.crosshair = "circle";
    settings.hitmarkers = "off";
    settings.vsync = false;
    settings.actionBlur = "off";
    Settings read;
    CHECK(applyVideoIni(read, videoIni(settings)) == 30);
    CHECK(videoIni(read) == videoIni(settings));
    CHECK(read.actionBlur == "off");

    Settings legacy;
    CHECK(applyVideoIni(legacy, "[video]\r\nSSAO = Medium\r\ninternal_height=999\r\nfov=20\r\nnonsense=1\r\n") == 1);
    CHECK(legacy.aoMethod == "ssao");
    CHECK(legacy.aoQuality == "medium");
    CHECK(legacy.internalHeight == 720);
    CHECK(legacy.verticalFov == 0);
}

TEST_CASE("Settings JSON round-trips, and bad fields keep their defaults") {
    Settings settings = defaultSettings();
    settings.gpuRenderer = false;
    settings.menuLanguage = "pt-BR";
    settings.mouseSensitivity = 2.5;
    settings.bindings = {{"FIRE", "Mouse2"}};
    const Settings read = settingsFromJson(settingsJson(settings));
    CHECK(settingsJson(read) == settingsJson(settings));

    const Settings repaired = settingsFromJson(
        R"({"Threads":"eight","FpsLimit":75,"MenuLanguage":"xx","VerticalFov":90,"SilverWeapons":true,
            "Bindings":{"FIRE":"Banana","AIM":"Mouse3"}})");
    CHECK(repaired.threads == defaultThreads());
    CHECK(repaired.fpsLimit == 0);
    CHECK(repaired.menuLanguage == "en-US");
    CHECK(repaired.verticalFov == 90);
    CHECK(repaired.silverWeaponMode == "force-on");
    CHECK(repaired.bindings == bindings::Overrides{{"AIM", "Mouse3"}});

    CHECK(settingsJson(settingsFromJson("not json")) == settingsJson(defaultSettings()));
}

TEST_CASE("Saving writes all three files, and video.ini and controls.ini win when loading") {
    const black::tests::TempFolder folder;
    folder.write("CMakeLists.txt", "\n");
    folder.write("scripts/generate.py", "\n");
    folder.write("local/controls.ini", "[controller]\nlayout=2\n");
    const auto project = ProjectLayout::locate(folder.path());
    REQUIRE(project.has_value());

    Settings settings = defaultSettings();
    settings.anisotropy = 4;
    settings.optional_secondaries = "on";
    settings.bindings = {{"RELOAD", "T"}};
    REQUIRE(saveSettings(*project, settings).ok());
    CHECK(fileText(folder.path() / "local/controls.ini").find("[controller]\nlayout=2") != std::string::npos);
    CHECK(loadSettings(*project).anisotropy == 4);
    CHECK(loadSettings(*project).optional_secondaries == "on");

    folder.write("local/video.ini", "[video]\naf=2\noptional_secondaries=off\n");
    folder.write("local/controls.ini", "[keyboard]\nRELOAD=Y\n");
    const Settings loaded = loadSettings(*project);
    CHECK(loaded.anisotropy == 2);
    CHECK(loaded.optional_secondaries == "off");
    CHECK(loaded.bindings == bindings::Overrides{{"RELOAD", "Y"}});
}

TEST_CASE("Aim mode validates defaults and persists through JSON, INI and files") {
    CHECK(defaultSettings().aimMode == "toggle");
    CHECK(settingsFromJson(R"({"Schema":3})").aimMode == "toggle");
    for (const std::string_view bad : {R"({"AimMode":"invalid"})", R"({"AimMode":true})", R"({"AimMode":1})"})
        CHECK(settingsFromJson(bad).aimMode == "toggle");
    const black::tests::TempFolder folder;
    folder.write("CMakeLists.txt", "\n");
    folder.write("scripts/generate.py", "\n");
    const auto project = ProjectLayout::locate(folder.path());
    REQUIRE(project.has_value());
    for (const std::string_view mode : choices::kAimModes) {
        Settings wanted = defaultSettings();
        wanted.aimMode = mode;
        CHECK(settingsFromJson(settingsJson(wanted)).aimMode == mode);
        Settings ini = defaultSettings();
        CHECK(applyVideoIni(ini, videoIni(wanted)) > 0);
        CHECK(ini.aimMode == mode);
        CHECK(applyVideoIni(ini, "aim_mode=invalid\naim_mode=1\n") == 0);
        CHECK(ini.aimMode == mode);
        REQUIRE(saveSettings(*project, wanted).ok());
        CHECK(fileText(folder.path() / "local/video.ini").find("aim_mode=" + std::string(mode)) != std::string::npos);
        CHECK(loadSettings(*project).aimMode == mode);
    }
    Settings legacy = defaultSettings();
    CHECK(applyVideoIni(legacy, "[video]\nvsync=0\n") == 1);
    CHECK(legacy.aimMode == "toggle");
    CHECK(applyVideoIni(legacy, "AIM_MODE = HoLd\n") == 1);
    CHECK(legacy.aimMode == "hold");
}
