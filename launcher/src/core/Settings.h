#pragma once

#include "core/InputBindings.h"
#include "core/ProjectLayout.h"
#include "core/Result.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace black::core {

/// Everything the player can set. Stored as before in local/: video.ini (shared with the game's Video Settings
/// page), controls.ini ([keyboard]; the game's Controls pages write it too) and xbox-launcher.json (the rest).
struct Settings {
    // Game
    bool gpuRenderer = true;
    bool precompileShaders = true;  // the game precompiles recorded shaders unless this is off
    bool smooth60 = true;
    int smoothHz = 0;  // with smooth60: 0 runs the game clock at 60 steps per second, else 120 or 240
    int threads = 4;
    bool skipMovies = false;
    bool subtitles = true;
    bool skipStartupVideos = false;
    bool infiniteAmmo = false;
    bool infiniteHealth = false;
    std::string silverWeaponMode = "native";  // native, force-on, force-off
    std::string textures = "original";        // original, hd-pack (the texture_pack_hd folder)
    std::string menuLanguage = "en-US";

    // Diagnostics
    bool visualTraceEnabled = false;
    std::string visualTraceFrames = "120-240/30";
    std::string kgpuDrawLogFrames;
    bool dspTraceEnabled = false;

    // Display (video.ini)
    int windowWidth = 1280;
    int windowHeight = 960;
    int internalHeight = 720;
    std::string windowMode = "windowed";
    std::string scaleMode = "fit";
    std::string presentFilter = "linear";
    bool vsync = true;
    int fpsLimit = 0;
    std::string cameraAspect = "original";
    double verticalFov = 0;  // 0 keeps the game's own camera
    std::string motionBlur = "original";
    std::string actionBlur = "original";  // BLACK's own reload and aim blur

    // Graphics (video.ini)
    std::string antiAliasing = "fxaa";
    int anisotropy = 16;
    std::string depthOfField = "off";
    std::string sharpening = "off";
    std::string aoMethod = "off";
    std::string aoQuality = "high";
    std::string reflections = "high";
    std::string rayTracing = "off";  // experimental; the game checks the GPU
    std::string crosshair = "original";
    std::string hitmarkers = "off";
    std::string optional_secondaries = "off";  // mission completion still requires primary objectives
    std::string sprint = "off";
    std::string jump = "off";
    std::string aimMode = "toggle";  // toggle preserves the original aim behavior
    int lod_distance = 100;          // percentages: 100 keeps the original distances/damage/health
    int bullet_damage = 100;
    int enemy_health = 100;
    std::string headshot_instakill = "off";

    // Input
    std::string inputMode = "auto";
    std::string promptMode = "auto";
    double mouseSensitivity = 1.0;
    bool invertY = false;
    bindings::Overrides bindings;
};

namespace choices {
inline constexpr std::array<int, 7> kFrameLimits = {0, 30, 60, 120, 144, 165, 240};
inline constexpr std::array<int, 5> kInternalHeights = {480, 720, 1080, 1440, 2160};
inline constexpr std::array<int, 5> kAnisotropy = {1, 2, 4, 8, 16};
inline constexpr std::array<int, 3> kSmoothHz = {0, 120, 240};
inline constexpr std::array<int, 6> kLodDistances = {100, 200, 300, 500, 800, 1000};
inline constexpr std::array<int, 8> kBulletDamages = {50, 100, 150, 200, 300, 400, 600, 1000};
inline constexpr std::array<int, 8> kEnemyHealths = {25, 50, 75, 100, 150, 200, 300, 400};
inline constexpr double kFovMin = 35.0;
inline constexpr double kFovMax = 120.0;
inline constexpr std::array<std::string_view, 2> kWindowModes = {"windowed", "borderless"};
inline constexpr std::array<std::string_view, 3> kScaleModes = {"fit", "stretch", "integer"};
inline constexpr std::array<std::string_view, 2> kFilters = {"linear", "nearest"};
inline constexpr std::array<std::string_view, 5> kAspects = {"original", "4:3", "16:9", "21:9", "32:9"};
inline constexpr std::array<std::string_view, 2> kMotionBlur = {"original", "off"};
inline constexpr std::array<std::string_view, 7> kAntiAliasing = {
    "off", "fxaa", "ssaa", "dlaa", "dlss_quality", "dlss_balanced", "dlss_performance"};
inline constexpr std::array<std::string_view, 4> kSteps = {"off", "low", "medium", "high"};
inline constexpr std::array<std::string_view, 5> kAoMethods = {"off", "ssao", "hbao", "hbao_plus", "gtao"};
inline constexpr std::array<std::string_view, 4> kAoQualities = {"low", "medium", "high", "ultra"};
inline constexpr std::array<std::string_view, 5> kReflections = {"original", "low", "medium", "high", "ultra"};
inline constexpr std::array<std::string_view, 2> kOnOff = {"off", "on"};
inline constexpr std::array<std::string_view, 2> kAimModes = {"toggle", "hold"};
inline constexpr std::array<std::string_view, 4> kCrosshairs = {"original", "cross", "t", "circle"};
inline constexpr std::array<std::string_view, 3> kSilverWeapons = {"native", "force-on", "force-off"};
inline constexpr std::array<std::string_view, 2> kTextures = {"original", "hd-pack"};
inline constexpr std::array<std::string_view, 3> kInputModes = {"auto", "keyboard_mouse", "controller"};
inline constexpr std::array<std::string_view, 6> kLanguages = {"en-US", "pt-BR", "es", "fr", "de", "it"};
}  // namespace choices

[[nodiscard]] int defaultThreads();
[[nodiscard]] Settings defaultSettings();

/// The picture the game renders: the internal height at the camera's aspect (4:3 for Original), even width.
[[nodiscard]] std::array<int, 2> renderSize(const Settings& settings);
/// What the GPU draws: renderSize, or twice each side under SSAA while within its limits.
[[nodiscard]] std::array<int, 2> rasterSize(const Settings& settings);

/// Why these settings cannot start the game (a diagnostic that needs another renderer, a bad frame range...).
[[nodiscard]] std::optional<std::string> settingsProblem(const Settings& settings);

[[nodiscard]] std::string videoIni(const Settings& settings);
/// The game's rules: unknown keys and invalid values are skipped. Returns how many keys applied.
int applyVideoIni(Settings& settings, std::string_view text);

[[nodiscard]] std::string settingsJson(const Settings& settings);
/// Fields of the wrong type or out of range keep their defaults; older files gain the new fields.
[[nodiscard]] Settings settingsFromJson(std::string_view json);
/// Apply named launcher settings as a batch. Invalid names, types and values produce an error.
[[nodiscard]] Result<Settings> withSettingAssignments(const Settings& saved, std::span<const std::string> assignments);

/// "120", "120-180" or "120-240/30", normalized; nothing when invalid.
[[nodiscard]] std::optional<std::string> normalizeTraceFrames(std::string_view value);
/// "frame" or "frame,count" (count 1-120), normalized; nothing when invalid.
[[nodiscard]] std::optional<std::string> normalizeDrawLogFrames(std::string_view value);

[[nodiscard]] Settings loadSettings(const ProjectLayout& project);
[[nodiscard]] Result<bool> saveSettings(const ProjectLayout& project, const Settings& settings);

}  // namespace black::core
