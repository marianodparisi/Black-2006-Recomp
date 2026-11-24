#include "TempFolder.h"
#include "app/SettingsFlow.h"
#include "app/Translations.h"
#include "core/Settings.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string_view>

namespace {

std::size_t row(const black::app::Page& page, std::string_view label) {
    const auto found =
        std::find_if(page.rows.begin(), page.rows.end(), [label](const auto& item) { return item.label == label; });
    REQUIRE(found != page.rows.end());
    return static_cast<std::size_t>(found - page.rows.begin());
}

}  // namespace

TEST_CASE("Tuning multipliers validate and preserve original defaults through JSON and INI") {
    using namespace black::core;
    const Settings defaults = settingsFromJson("{}");
    CHECK(defaults.lod_distance == 100);
    CHECK(defaults.bullet_damage == 100);
    CHECK(defaults.enemy_health == 100);
    CHECK(defaults.headshot_instakill == "off");
    const auto verify = [](const auto& values, int Settings::* field) {
        for (const int choice : values) {
            Settings wanted;
            wanted.*field = choice;
            CHECK(settingsFromJson(settingsJson(wanted)).*field == choice);
            Settings read;
            CHECK(applyVideoIni(read, videoIni(wanted)) == 30);
            CHECK(read.*field == choice);
        }
    };
    verify(choices::kLodDistances, &Settings::lod_distance);
    verify(choices::kBulletDamages, &Settings::bullet_damage);
    verify(choices::kEnemyHealths, &Settings::enemy_health);
    const Settings invalid =
        settingsFromJson(R"({"LodDistance":150,"BulletDamage":"200","EnemyHealth":true,"HeadshotInstakill":1})");
    CHECK(invalid.lod_distance == 100);
    CHECK(invalid.bullet_damage == 100);
    CHECK(invalid.enemy_health == 100);
    CHECK(invalid.headshot_instakill == "off");
    Settings read;
    CHECK(applyVideoIni(read, "lod_distance=150\nbullet_damage=0\nenemy_health=-25\nheadshot_instakill=1\n") == 0);
    CHECK(read.lod_distance == 100);
    CHECK(read.bullet_damage == 100);
    CHECK(read.enemy_health == 100);
    CHECK(read.headshot_instakill == "off");
    CHECK(applyVideoIni(read, "LOD_DISTANCE=1000\nBULLET_DAMAGE=50\nENEMY_HEALTH=25\nHEADSHOT_INSTAKILL=On\n") == 4);
    CHECK(read.lod_distance == 1000);
    CHECK(read.bullet_damage == 50);
    CHECK(read.enemy_health == 25);
    CHECK(settingsFromJson(settingsJson(read)).headshot_instakill == "on");
}

TEST_CASE("Graphics and Gameplay tuning pages persist and reload independent choices") {
    const black::tests::TempFolder folder;
    const black::core::ProjectLayout project(folder.path());
    black::app::setLanguage("en-US");
    black::app::SettingsFlow flow(project);
    flow.handle({row(flow.page(), "GRAPHICS"), 0});
    auto page = flow.page();
    CHECK(page.rows[row(page, "LOD DISTANCE")].value == "1X");
    flow.handle({row(page, "LOD DISTANCE"), 1});
    CHECK(flow.page().rows[row(page, "LOD DISTANCE")].value == "2X");
    flow.back();
    flow.handle({row(flow.page(), "GAMEPLAY"), 0});
    page = flow.page();
    CHECK(page.rows[row(page, "BULLET DAMAGE")].value == "1X");
    CHECK(page.rows[row(page, "ENEMY HEALTH")].value == "1X");
    CHECK(page.rows[row(page, "INSTANT-KILL HEADSHOTS")].value == "OFF");
    flow.handle({row(page, "BULLET DAMAGE"), 1});
    flow.handle({row(page, "ENEMY HEALTH"), -1});
    flow.handle({row(page, "INSTANT-KILL HEADSHOTS"), 1});
    CHECK(flow.page().rows[row(page, "BULLET DAMAGE")].value == "1.5X");
    CHECK(flow.page().rows[row(page, "ENEMY HEALTH")].value == "0.75X");
    CHECK(flow.page().rows[row(page, "INSTANT-KILL HEADSHOTS")].value == "ON");
    auto saved = black::core::loadSettings(project);
    CHECK(saved.lod_distance == 200);
    CHECK(saved.bullet_damage == 150);
    CHECK(saved.enemy_health == 75);
    CHECK(saved.headshot_instakill == "on");
    saved.enemy_health = 25;
    saved.headshot_instakill = "off";
    REQUIRE(black::core::saveSettings(project, saved).ok());
    flow.reload();
    CHECK(flow.page().rows[row(page, "ENEMY HEALTH")].value == "0.25X");
    CHECK(flow.page().rows[row(page, "INSTANT-KILL HEADSHOTS")].value == "OFF");
}

TEST_CASE("HD pack folder is created and opened only by the explicit folder action") {
    const black::tests::TempFolder folder;
    const black::core::ProjectLayout project(folder.path());
    black::app::setLanguage("en-US");
    unsigned opens = 0;
    black::app::SettingsFlow flow(project, [&](const std::filesystem::path& path) -> black::core::Result<bool> {
        ++opens;
        CHECK(path == project.root() / "texture_pack_hd");
        CHECK(std::filesystem::is_directory(path));
        return true;
    });
    flow.handle({row(flow.page(), "GAME"), 0});
    auto page = flow.page();
    CHECK(page.rows[row(page, "HD PACK FOLDER")].value == "MISSING");
    CHECK_FALSE(std::filesystem::exists(project.root() / "texture_pack_hd"));
    CHECK(opens == 0);
    flow.handle({row(page, "OPEN HD PACK FOLDER"), 0});
    CHECK(opens == 1);
    CHECK(flow.page().rows[row(page, "HD PACK FOLDER")].value == "FOLDER FOUND");
    CHECK_FALSE(flow.page().noticeIsProblem);
    CHECK(black::core::loadSettings(project).textures == "original");

    black::app::SettingsFlow failed(project, [](const std::filesystem::path&) -> black::core::Result<bool> {
        return black::core::Error{"The file manager could not be opened."};
    });
    failed.handle({row(failed.page(), "GAME"), 0});
    failed.handle({row(failed.page(), "OPEN HD PACK FOLDER"), 0});
    CHECK(failed.page().noticeIsProblem);
    CHECK(failed.page().notice == "The file manager could not be opened.");
}

TEST_CASE("Infinite health is an independent persisted Game setting") {
    const black::tests::TempFolder folder;
    const black::core::ProjectLayout project(folder.path());
    black::app::setLanguage("en-US");
    black::app::SettingsFlow flow(project);
    flow.handle({row(flow.page(), "GAME"), 0});
    const auto health = row(flow.page(), "INFINITE HEALTH");
    CHECK(flow.page().rows[health].value == "OFF");
    flow.handle({health, 1});
    CHECK(flow.page().rows[health].value == "ON");
    const auto saved = black::core::loadSettings(project);
    CHECK(saved.infiniteHealth);
    CHECK_FALSE(saved.infiniteAmmo);
    flow.reload();
    CHECK(flow.page().rows[health].value == "ON");
    flow.handle({health, -1});
    CHECK_FALSE(black::core::loadSettings(project).infiniteHealth);
}
