#include "core/Settings.h"

#include "core/Files.h"
#include "core/ProjectLock.h"
#include "core/Text.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>

namespace black::core {
namespace {

using Json = nlohmann::ordered_json;

template <typename List, typename Value>
bool contains(const List& list, const Value& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<long long> parseDigits(std::string_view text) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    long long value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parseNumber(std::string_view text) {
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    double value = 0;
    stream >> value;
    if (!stream || !stream.eof() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

bool validFov(double fov) {
    return fov == 0.0 || (std::isfinite(fov) && fov >= choices::kFovMin && fov <= choices::kFovMax);
}
bool validSensitivity(double value) {
    return std::isfinite(value) && value >= bindings::kSensitivityMin && value <= bindings::kSensitivityMax;
}

std::string formatNumber(double value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return {buffer.data(), result.ptr};
}

template <typename T, typename Check>
void readField(const Json& json, const char* name, T& out, Check valid) {
    const auto it = json.find(name);
    if (it == json.end()) {
        return;
    }
    if constexpr (std::is_same_v<T, bool>) {
        if (it->is_boolean() && valid(it->template get<bool>())) {
            out = it->template get<bool>();
        }
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (it->is_string() && valid(it->template get<std::string>())) {
            out = it->template get<std::string>();
        }
    } else if constexpr (std::is_same_v<T, int>) {
        if (it->is_number_integer() && valid(it->template get<long long>())) {
            out = static_cast<int>(it->template get<long long>());
        }
    } else {
        if (it->is_number() && valid(it->template get<double>())) {
            out = it->template get<double>();
        }
    }
}

const auto kAny = [](const auto&) { return true; };

}  // namespace

int defaultThreads() {
    const unsigned logical = std::thread::hardware_concurrency();
    return std::clamp(static_cast<int>(logical / 2), 1, 16);
}

Settings defaultSettings() {
    Settings settings;
    settings.threads = defaultThreads();
    return settings;
}

std::array<int, 2> renderSize(const Settings& settings) {
    int num = 4;
    int den = 3;
    if (settings.cameraAspect == "16:9") {
        num = 16;
        den = 9;
    } else if (settings.cameraAspect == "21:9") {
        num = 21;
        den = 9;
    } else if (settings.cameraAspect == "32:9") {
        num = 32;
        den = 9;
    }
    int width = static_cast<int>((static_cast<long long>(settings.internalHeight) * num + den / 2) / den);
    width = (width + 1) & ~1;
    return {std::max(width, 640), std::max(settings.internalHeight, 480)};
}

std::array<int, 2> rasterSize(const Settings& settings) {
    const std::array<int, 2> size = renderSize(settings);
    constexpr long long kPixelLimit = 16777216;
    if (settings.antiAliasing == "ssaa" && size[0] * 2 <= 8192 && size[1] * 2 <= 8192 &&
        static_cast<long long>(size[0]) * 2 * size[1] * 2 <= kPixelLimit) {
        return {size[0] * 2, size[1] * 2};
    }
    return size;
}

std::optional<std::string> normalizeTraceFrames(std::string_view value) {
    const std::string_view text = trim(value);
    const std::size_t slash = text.find('/');
    const std::string_view range = text.substr(0, slash);
    if (slash != std::string_view::npos && text.find('/', slash + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t dash = range.find('-');
    if (dash != std::string_view::npos && range.find('-', dash + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    if (slash != std::string_view::npos && dash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto first = parseDigits(range.substr(0, dash));
    const auto last = dash == std::string_view::npos ? first : parseDigits(range.substr(dash + 1));
    const auto step =
        slash == std::string_view::npos ? std::optional<long long>(1) : parseDigits(text.substr(slash + 1));
    if (!first || !last || !step || *first <= 0 || *last <= 0 || *step <= 0 || *last < *first) {
        return std::nullopt;
    }
    std::string normalized = std::to_string(*first);
    if (dash != std::string_view::npos) {
        normalized += "-" + std::to_string(*last);
    }
    if (slash != std::string_view::npos) {
        normalized += "/" + std::to_string(*step);
    }
    return normalized;
}

std::optional<std::string> normalizeDrawLogFrames(std::string_view value) {
    const std::string_view text = trim(value);
    const std::size_t comma = text.find(',');
    if (comma != std::string_view::npos && text.find(',', comma + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto first = parseDigits(text.substr(0, comma));
    const auto count =
        comma == std::string_view::npos ? std::optional<long long>(1) : parseDigits(text.substr(comma + 1));
    if (!first || !count || *first <= 0 || *first > 2147483647 || *count <= 0 || *count > 120) {
        return std::nullopt;
    }
    return std::to_string(*first) + (comma == std::string_view::npos ? "" : "," + std::to_string(*count));
}

std::optional<std::string> settingsProblem(const Settings& settings) {
    if (settings.visualTraceEnabled) {
        if (settings.gpuRenderer) {
            return "The detailed visual trace needs the CPU renderer. Turn off the GPU renderer for this diagnostic "
                   "run.";
        }
        if (!normalizeTraceFrames(settings.visualTraceFrames)) {
            return "Visual trace frames: enter a frame (120), a range (120-180) or a stepped range (120-240/30).";
        }
    }
    if (!settings.kgpuDrawLogFrames.empty()) {
        if (!settings.gpuRenderer) {
            return "The GPU draw log needs the GPU renderer.";
        }
        if (!normalizeDrawLogFrames(settings.kgpuDrawLogFrames)) {
            return "GPU draw log: enter a frame or frame,count (1-120 frames).";
        }
    }
    return std::nullopt;
}

std::string videoIni(const Settings& s) {
    std::string text =
        "; BLACK video settings. Edited by the in-game Video Settings page and by the launcher.\n[video]\n";
    const auto line = [&text](std::string_view key, const std::string& value) {
        text.append(key).append("=").append(value).push_back('\n');
    };
    line("internal_height", std::to_string(s.internalHeight));
    line("window_width", std::to_string(s.windowWidth));
    line("window_height", std::to_string(s.windowHeight));
    line("window_mode", s.windowMode);
    line("scale", s.scaleMode);
    line("filter", s.presentFilter);
    line("vsync", s.vsync ? "1" : "0");
    line("fps_limit", std::to_string(s.fpsLimit));
    line("aspect", s.cameraAspect);
    line("fov", formatNumber(s.verticalFov));
    line("motion_blur", s.motionBlur);
    line("action_blur", s.actionBlur);
    line("aa", s.antiAliasing);
    line("af", std::to_string(s.anisotropy));
    line("dof", s.depthOfField);
    line("sharpen", s.sharpening);
    line("ao_method", s.aoMethod);
    line("ao_quality", s.aoQuality);
    line("reflections", s.reflections);
    line("ray_tracing", s.rayTracing);
    line("crosshair", s.crosshair);
    line("hitmarkers", s.hitmarkers);
    line("optional_secondaries", s.optional_secondaries);
    line("lod_distance", std::to_string(s.lod_distance));
    line("bullet_damage", std::to_string(s.bullet_damage));
    line("enemy_health", std::to_string(s.enemy_health));
    line("headshot_instakill", s.headshot_instakill);
    line("aim_mode", s.aimMode);
    line("sprint", s.sprint);
    line("jump", s.jump);
    return text;
}

int applyVideoIni(Settings& s, std::string_view text) {
    int applied = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string_view line = trim(text.substr(start, end == std::string_view::npos ? text.npos : end - start));
        start = end == std::string_view::npos ? text.size() + 1 : end + 1;
        if (line.empty() || line.front() == ';' || line.front() == '#' || line.front() == '[') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string key = lower(trim(line.substr(0, equals)));
        const std::string_view raw = trim(line.substr(equals + 1));
        const std::string value = lower(raw);
        const auto number = parseDigits(raw);
        const auto pick = [&](auto list, std::string& out) {
            if (contains(list, value)) {
                out = value;
                ++applied;
            }
        };
        if (key == "internal_height") {
            if (number && contains(choices::kInternalHeights, *number)) {
                s.internalHeight = static_cast<int>(*number);
                ++applied;
            }
        } else if (key == "window_width") {
            if (number && *number >= 320 && *number <= 7680) {
                s.windowWidth = static_cast<int>(*number);
                ++applied;
            }
        } else if (key == "window_height") {
            if (number && *number >= 240 && *number <= 4320) {
                s.windowHeight = static_cast<int>(*number);
                ++applied;
            }
        } else if (key == "window_mode") {
            pick(choices::kWindowModes, s.windowMode);
        } else if (key == "scale") {
            pick(choices::kScaleModes, s.scaleMode);
        } else if (key == "filter") {
            pick(choices::kFilters, s.presentFilter);
        } else if (key == "vsync") {
            if (value == "0" || value == "1") {
                s.vsync = value == "1";
                ++applied;
            }
        } else if (key == "fps_limit") {
            if (number && contains(choices::kFrameLimits, *number)) {
                s.fpsLimit = static_cast<int>(*number);
                ++applied;
            }
        } else if (key == "aspect") {
            pick(choices::kAspects, s.cameraAspect);
        } else if (key == "fov") {
            if (const auto fov = parseNumber(raw); fov && validFov(*fov)) {
                s.verticalFov = *fov;
                ++applied;
            }
        } else if (key == "motion_blur") {
            pick(choices::kMotionBlur, s.motionBlur);
        } else if (key == "action_blur") {
            pick(choices::kMotionBlur, s.actionBlur);
        } else if (key == "aa") {
            pick(choices::kAntiAliasing, s.antiAliasing);
        } else if (key == "af") {
            if (number && contains(choices::kAnisotropy, *number)) {
                s.anisotropy = static_cast<int>(*number);
                ++applied;
            }
        } else if (key == "dof") {
            pick(choices::kSteps, s.depthOfField);
        } else if (key == "sharpen") {
            pick(choices::kSteps, s.sharpening);
        } else if (key == "ao_method") {
            pick(choices::kAoMethods, s.aoMethod);
        } else if (key == "ao_quality") {
            pick(choices::kAoQualities, s.aoQuality);
        } else if (key == "reflections") {
            pick(choices::kReflections, s.reflections);
        } else if (key == "ray_tracing") {
            pick(choices::kOnOff, s.rayTracing);
        } else if (key == "crosshair") {
            pick(choices::kCrosshairs, s.crosshair);
        } else if (key == "hitmarkers") {
            pick(choices::kOnOff, s.hitmarkers);
        } else if (key == "optional_secondaries") {
            pick(choices::kOnOff, s.optional_secondaries);
        } else if (key == "sprint") {
            pick(choices::kOnOff, s.sprint);
        } else if (key == "jump") {
            pick(choices::kOnOff, s.jump);
        } else if (key == "aim_mode") {
            pick(choices::kAimModes, s.aimMode);
        } else if (key == "lod_distance") {
            if (number && contains(choices::kLodDistances, *number)) {
                s.lod_distance = static_cast<int>(*number);
                ++applied;
            }
        } else if (key == "bullet_damage") {
            if (number && contains(choices::kBulletDamages, *number)) {
                s.bullet_damage = static_cast<int>(*number);
                ++applied;
            }
        } else if (key == "enemy_health") {
            if (number && contains(choices::kEnemyHealths, *number)) {
                s.enemy_health = static_cast<int>(*number);
                ++applied;
            }
        } else if (key == "headshot_instakill") {
            pick(choices::kOnOff, s.headshot_instakill);
        } else if (key == "ssao") {  // the old single setting: method SSAO at a quality, or off
            if (value == "off") {
                s.aoMethod = "off";
                ++applied;
            } else if (contains(choices::kAoQualities, value)) {
                s.aoMethod = "ssao";
                s.aoQuality = value;
                ++applied;
            }
        }
    }
    return applied;
}

std::string settingsJson(const Settings& s) {
    Json json;
    json["SettingsVersion"] = 14;
    json["GpuRenderer"] = s.gpuRenderer;
    json["PrecompileShaders"] = s.precompileShaders;
    json["Smooth60"] = s.smooth60;
    json["SmoothHz"] = s.smoothHz;
    json["Threads"] = s.threads;
    json["SkipMovies"] = s.skipMovies;
    json["Subtitles"] = s.subtitles;
    json["SkipStartupVideos"] = s.skipStartupVideos;
    json["InfiniteAmmo"] = s.infiniteAmmo;
    json["InfiniteHealth"] = s.infiniteHealth;
    json["SilverWeaponMode"] = s.silverWeaponMode;
    json["Textures"] = s.textures;
    json["MenuLanguage"] = s.menuLanguage;
    json["VisualTraceEnabled"] = s.visualTraceEnabled;
    json["VisualTraceFrames"] = s.visualTraceFrames;
    json["KgpuDrawLogFrames"] = s.kgpuDrawLogFrames;
    json["DspTraceEnabled"] = s.dspTraceEnabled;
    json["OutputWidth"] = s.windowWidth;
    json["OutputHeight"] = s.windowHeight;
    json["InternalHeight"] = s.internalHeight;
    json["AntiAliasing"] = s.antiAliasing;
    json["Anisotropy"] = s.anisotropy;
    json["DepthOfField"] = s.depthOfField;
    json["Sharpening"] = s.sharpening;
    json["AoMethod"] = s.aoMethod;
    json["AoQuality"] = s.aoQuality;
    json["Reflections"] = s.reflections;
    json["RayTracing"] = s.rayTracing;
    json["Crosshair"] = s.crosshair;
    json["HitMarkers"] = s.hitmarkers;
    json["OptionalSecondaries"] = s.optional_secondaries;
    json["LodDistance"] = s.lod_distance;
    json["BulletDamage"] = s.bullet_damage;
    json["EnemyHealth"] = s.enemy_health;
    json["HeadshotInstakill"] = s.headshot_instakill;
    json["AimMode"] = s.aimMode;
    json["Sprint"] = s.sprint;
    json["Jump"] = s.jump;
    json["WindowMode"] = s.windowMode;
    json["ScaleMode"] = s.scaleMode;
    json["PresentFilter"] = s.presentFilter;
    json["VSync"] = s.vsync;
    json["FpsLimit"] = s.fpsLimit;
    json["CameraAspect"] = s.cameraAspect;
    json["VerticalFov"] = s.verticalFov;
    json["MotionBlur"] = s.motionBlur;
    json["ActionBlur"] = s.actionBlur;
    json["InputMode"] = s.inputMode;
    json["PromptMode"] = s.promptMode;
    json["MouseSensitivity"] = s.mouseSensitivity;
    json["InvertY"] = s.invertY;
    json["Bindings"] = Json::object();
    for (const auto& [action, keys] : s.bindings) {
        json["Bindings"][action] = keys;
    }
    return json.dump(2) + "\n";
}

Settings settingsFromJson(std::string_view text) {
    Settings s = defaultSettings();
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object()) {
        return s;
    }
    const auto among = [](const auto& list) { return [&list](const std::string& v) { return contains(list, v); }; };
    readField(json, "GpuRenderer", s.gpuRenderer, kAny);
    readField(json, "PrecompileShaders", s.precompileShaders, kAny);
    readField(json, "Smooth60", s.smooth60, kAny);
    readField(json, "SmoothHz", s.smoothHz, [](long long v) { return contains(choices::kSmoothHz, v); });
    readField(json, "Threads", s.threads, [](long long v) { return v >= 1 && v <= 64; });
    readField(json, "SkipMovies", s.skipMovies, kAny);
    readField(json, "Subtitles", s.subtitles, kAny);
    // Replace legacy timed controller presses with direct logo-video skipping.
    // An explicit new setting wins, including false or an invalid value.
    readField(json, json.contains("SkipStartupVideos") ? "SkipStartupVideos" : "AdvanceMenus", s.skipStartupVideos,
              kAny);
    readField(json, "InfiniteAmmo", s.infiniteAmmo, kAny);
    readField(json, "InfiniteHealth", s.infiniteHealth, kAny);
    readField(json, "SilverWeaponMode", s.silverWeaponMode, among(choices::kSilverWeapons));
    readField(json, "Textures", s.textures, among(choices::kTextures));
    if (!json.contains("SilverWeaponMode") && json.value("SilverWeapons", false)) {
        s.silverWeaponMode = "force-on";  // the old on/off field
    }
    readField(json, "MenuLanguage", s.menuLanguage, among(choices::kLanguages));
    readField(json, "VisualTraceEnabled", s.visualTraceEnabled, kAny);
    readField(json, "VisualTraceFrames", s.visualTraceFrames,
              [](const std::string& v) { return normalizeTraceFrames(v).has_value(); });
    readField(json, "KgpuDrawLogFrames", s.kgpuDrawLogFrames,
              [](const std::string& v) { return v.empty() || normalizeDrawLogFrames(v).has_value(); });
    readField(json, "DspTraceEnabled", s.dspTraceEnabled, kAny);
    readField(json, "OutputWidth", s.windowWidth, [](long long v) { return v >= 320 && v <= 7680; });
    readField(json, "OutputHeight", s.windowHeight, [](long long v) { return v >= 240 && v <= 4320; });
    readField(json, "InternalHeight", s.internalHeight,
              [](long long v) { return contains(choices::kInternalHeights, v); });
    readField(json, "AntiAliasing", s.antiAliasing, among(choices::kAntiAliasing));
    readField(json, "Anisotropy", s.anisotropy, [](long long v) { return contains(choices::kAnisotropy, v); });
    readField(json, "DepthOfField", s.depthOfField, among(choices::kSteps));
    readField(json, "Sharpening", s.sharpening, among(choices::kSteps));
    readField(json, "AoMethod", s.aoMethod, among(choices::kAoMethods));
    readField(json, "AoQuality", s.aoQuality, among(choices::kAoQualities));
    readField(json, "Reflections", s.reflections, among(choices::kReflections));
    readField(json, "RayTracing", s.rayTracing, among(choices::kOnOff));
    readField(json, "Crosshair", s.crosshair, among(choices::kCrosshairs));
    readField(json, "HitMarkers", s.hitmarkers, among(choices::kOnOff));
    readField(json, "OptionalSecondaries", s.optional_secondaries, among(choices::kOnOff));
    readField(json, "LodDistance", s.lod_distance, [](long long v) { return contains(choices::kLodDistances, v); });
    readField(json, "BulletDamage", s.bullet_damage, [](long long v) { return contains(choices::kBulletDamages, v); });
    readField(json, "EnemyHealth", s.enemy_health, [](long long v) { return contains(choices::kEnemyHealths, v); });
    readField(json, "HeadshotInstakill", s.headshot_instakill, among(choices::kOnOff));
    readField(json, "Sprint", s.sprint, among(choices::kOnOff));
    readField(json, "Jump", s.jump, among(choices::kOnOff));
    readField(json, "AimMode", s.aimMode, among(choices::kAimModes));
    readField(json, "WindowMode", s.windowMode, among(choices::kWindowModes));
    readField(json, "ScaleMode", s.scaleMode, among(choices::kScaleModes));
    readField(json, "PresentFilter", s.presentFilter, among(choices::kFilters));
    readField(json, "VSync", s.vsync, kAny);
    readField(json, "FpsLimit", s.fpsLimit, [](long long v) { return contains(choices::kFrameLimits, v); });
    readField(json, "CameraAspect", s.cameraAspect, among(choices::kAspects));
    readField(json, "VerticalFov", s.verticalFov, [](double v) { return validFov(v); });
    readField(json, "MotionBlur", s.motionBlur, among(choices::kMotionBlur));
    readField(json, "ActionBlur", s.actionBlur, among(choices::kMotionBlur));
    readField(json, "InputMode", s.inputMode, among(choices::kInputModes));
    readField(json, "PromptMode", s.promptMode, among(choices::kInputModes));
    readField(json, "MouseSensitivity", s.mouseSensitivity, [](double v) { return validSensitivity(v); });
    readField(json, "InvertY", s.invertY, kAny);
    if (const auto it = json.find("Bindings"); it != json.end() && it->is_object()) {
        bindings::Overrides overrides;
        for (const auto& [action, keys] : it->items()) {
            if (keys.is_string()) {
                overrides[action] = keys.get<std::string>();
            }
        }
        s.bindings = bindings::repair(overrides);
    }
    return s;
}

namespace {

Result<Settings> withSettingAssignments(const Settings& saved, std::span<const std::string> assignments) {
    // Compare binding maps by key, independent of the order used on the command line.
    using Values = nlohmann::json;
    Values requested = Values::parse(settingsJson(saved));
    for (const auto& assignment : assignments) {
        const auto equals = assignment.find('=');
        const auto key = assignment.substr(0, equals);
        if (equals == std::string::npos || key == "SettingsVersion" || !requested.contains(key)) {
            return Error{"Expected a setting from --settings in the form Name=value: " + assignment};
        }
        const auto value = assignment.substr(equals + 1);
        auto& field = requested[key];
        Values parsed = field.is_string() ? Values(value) : Values::parse(value, nullptr, false);
        if (parsed.is_discarded() || (field.is_number_integer() && !parsed.is_number_integer())) {
            return Error{"Invalid value for " + key};
        }
        if (key == "Bindings") {
            if (!parsed.is_object()) {
                return Error{"Bindings must be an object of action names and keys."};
            }
            bindings::Overrides overrides;
            for (const auto& [name, keys] : parsed.items()) {
                const auto* action = bindings::findAction(name);
                if (action == nullptr || !keys.is_string()) {
                    return Error{"Invalid binding for " + name};
                }
                const auto& valueText = keys.get_ref<const std::string&>();
                if (const auto problem = bindings::problem(valueText)) {
                    return Error{"Invalid binding for " + name + ": " + *problem};
                }
                if (!overrides.emplace(std::string(action->name), valueText).second) {
                    return Error{"Duplicate binding for " + name};
                }
            }
            parsed = bindings::repair(overrides);  // explicit defaults are stored by removing the override
        }
        field = std::move(parsed);
        // The existing settings reader validates types, ranges, enum choices and binding names.
        // A rejected field falls back to its default. Reject that substitution on the command line.
        const Settings checked = settingsFromJson(requested.dump());
        if (Values::parse(settingsJson(checked)).at(key) != field) {
            return Error{"Invalid value for " + key};
        }
    }
    Settings settings = settingsFromJson(requested.dump());
    if (const auto problem = settingsProblem(settings)) {
        return Error{*problem};
    }
    return settings;
}

Settings readSettings(const ProjectLayout& project) {
    const std::filesystem::path local = project.localDirectory();
    Settings settings = defaultSettings();
    if (auto json = readFile(local / "xbox-launcher.json")) {
        settings = settingsFromJson(std::string_view(reinterpret_cast<const char*>(json.value().data()),  // NOLINT
                                                     json.value().size()));
    }
    if (auto video = readFile(local / "video.ini")) {
        applyVideoIni(settings, std::string_view(reinterpret_cast<const char*>(video.value().data()),
                                                 video.value().size()));  // NOLINT
    }
    if (auto controls = readFile(local / "controls.ini")) {
        const std::string_view text(reinterpret_cast<const char*>(controls.value().data()),  // NOLINT
                                    controls.value().size());
        if (auto keyboard = bindings::fromControlsIni(text)) {
            settings.bindings = std::move(*keyboard);
        }
    }
    return settings;
}

Result<bool> writeSettings(const ProjectLayout& project, const Settings& settings) {
    const std::filesystem::path local = project.localDirectory();
    std::error_code error;
    std::filesystem::create_directories(local, error);
    std::string controls;
    if (auto existing = readFile(local / "controls.ini")) {
        controls.assign(reinterpret_cast<const char*>(existing.value().data()), existing.value().size());  // NOLINT
    }
    const std::array<std::pair<const char*, std::string>, 3> files = {{
        {"xbox-launcher.json", settingsJson(settings)},
        {"video.ini", videoIni(settings)},
        {"controls.ini", bindings::toControlsIni(controls, settings.bindings)},
    }};
    for (const auto& [name, contents] : files) {
        // Written beside and renamed over, so a crash never leaves half a file the game would read
        const std::filesystem::path target = local / name;
        const std::filesystem::path temporary = local / (std::string(name) + ".launcher.tmp");
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!out) {
                return Error{"Could not save " + toUtf8(target)};
            }
        }
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return Error{"Could not save " + toUtf8(target)};
        }
    }
    return true;
}

}  // namespace

Settings loadSettings(const ProjectLayout& project) {
    const auto lock = ProjectLock::acquire(project, ProjectLock::Use::Settings);
    return readSettings(project);
}

Result<bool> saveSettings(const ProjectLayout& project, const Settings& settings) {
    const auto lock = ProjectLock::acquire(project, ProjectLock::Use::Settings);
    if (!lock) {
        return Error{"Could not lock the project settings."};
    }
    return writeSettings(project, settings);
}

Result<bool> updateSettings(const ProjectLayout& project, std::span<const std::string> assignments) {
    const auto lock = ProjectLock::acquire(project, ProjectLock::Use::Settings);
    if (!lock) {
        return Error{"Could not lock the project settings."};
    }
    const auto changed = withSettingAssignments(readSettings(project), assignments);
    if (!changed) {
        return changed.error();
    }
    return writeSettings(project, changed.value());
}

}  // namespace black::core
