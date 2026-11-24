#include "TempFolder.h"
#include "XisoBuilder.h"
#include "core/DiscImport.h"
#include "core/Xdvdfs.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stop_token>
#include <string>

using namespace black::core;
using black::tests::TempFolder;
using black::tests::XisoBuilder;

namespace {

std::string_view asText(const std::vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()),
            bytes.size()};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

std::string readText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// A disc like BLACK's: default.xbe at the root, data in folders, one file larger than a copy chunk.
XisoBuilder sampleDisc() {
    XisoBuilder disc;
    disc.addFile("default.xbe", "XBEH pretend executable");
    disc.addFile("GlobData.bin", std::string(10000, 'g'));
    disc.addFile("levels/m01/level.bin", "level one");
    disc.addFile("levels/m02/level.bin", "level two");
    disc.addFile("sound/music.bin", std::string(5 * 1024 * 1024 + 7, 'm'));
    disc.addFile("videos/intro.bin", "intro");
    for (int i = 0; i < 120; ++i) {  // enough names to spill the root table into a second sector
        disc.addFile("data/file_with_a_long_name_number_" + std::to_string(i) + ".dat", std::to_string(i));
    }
    return disc;
}

/// A source that can be imported in tests: the real check needs the retail default.xbe, so it is taken as
/// supported here; the import still verifies the copy against the disc's own hash.
DiscSource importable(const std::filesystem::path& location) {
    Result<DiscSource> source = inspectDiscSource(location);
    REQUIRE(source.ok());
    DiscSource value = std::move(source).value();
    CHECK(value.release.state == GameFilesState::UnsupportedVersion);
    value.release.state = GameFilesState::Supported;
    return value;
}

}  // namespace

TEST_CASE("An XISO lists its files and folders and reads them back") {
    const TempFolder folder;
    folder.write("disc.iso", asText(sampleDisc().build()));
    const auto image = xdvdfs::Image::open(folder.path() / "disc.iso");
    REQUIRE(image.ok());
    CHECK(image.value().partitionOffset() == 0);
    const auto entries = image.value().list();
    REQUIRE(entries.ok());

    const auto find = [&](const std::string& path) {
        return std::find_if(entries.value().begin(), entries.value().end(),
                            [&](const xdvdfs::Entry& entry) { return entry.path == path; });
    };
    REQUIRE(find("levels") != entries.value().end());
    CHECK(find("levels")->directory);
    REQUIRE(find("levels/m02/level.bin") != entries.value().end());
    CHECK(find("sound/music.bin")->size == 5 * 1024 * 1024 + 7);
    CHECK(find("data/file_with_a_long_name_number_119.dat") != entries.value().end());
    CHECK(find("levels") < find("levels/m01/level.bin"));

    const auto bytes = image.value().read(*find("levels/m02/level.bin"));
    REQUIRE(bytes.ok());
    CHECK(asText(bytes.value()) == "level two");
}

TEST_CASE("The game partition of a full disc dump is found at its offset") {
    const TempFolder folder;
    constexpr std::size_t kOffset = 0x30000;
    folder.write("redump.iso", asText(sampleDisc().build(kOffset)));
    const std::array<std::uint64_t, 2> offsets = {0, kOffset};
    const auto image = xdvdfs::Image::open(folder.path() / "redump.iso", offsets);
    REQUIRE(image.ok());
    CHECK(image.value().partitionOffset() == kOffset);
    CHECK(image.value().list().ok());
}

TEST_CASE("Files that are not Xbox images, and damaged images, are refused") {
    const TempFolder folder;
    folder.write("photo.iso", std::string(0x20000, 'x'));
    CHECK_FALSE(xdvdfs::Image::open(folder.path() / "photo.iso").ok());
    CHECK_FALSE(xdvdfs::Image::open(folder.path() / "missing.iso").ok());

    std::vector<std::uint8_t> truncated = sampleDisc().build();
    truncated.resize(truncated.size() - 2 * XisoBuilder::kSector);
    folder.write("truncated.iso", asText(truncated));
    const auto image = xdvdfs::Image::open(folder.path() / "truncated.iso");
    REQUIRE(image.ok());
    const auto entries = image.value().list();
    REQUIRE_FALSE(entries.ok());
    CHECK(entries.error().message.find("incomplete") != std::string::npos);
}

TEST_CASE("Names that could write outside the game folder are refused") {
    CHECK(xdvdfs::isSafeName("default.xbe"));
    CHECK_FALSE(xdvdfs::isSafeName(".."));
    CHECK_FALSE(xdvdfs::isSafeName("."));
    CHECK_FALSE(xdvdfs::isSafeName(""));
    CHECK_FALSE(xdvdfs::isSafeName("a/b"));
    CHECK_FALSE(xdvdfs::isSafeName("a\\b"));
    CHECK_FALSE(xdvdfs::isSafeName("C:"));

    const TempFolder folder;
    XisoBuilder disc;
    disc.addFile("default.xbe", "x");
    disc.addFile("..", "escape");
    folder.write("evil.iso", asText(disc.build()));
    const auto image = xdvdfs::Image::open(folder.path() / "evil.iso");
    REQUIRE(image.ok());
    CHECK_FALSE(image.value().list().ok());
}

TEST_CASE("Inspecting a disc image reports its release, size and file count without copying") {
    const TempFolder folder;
    folder.write("disc.iso", asText(sampleDisc().build()));
    const auto source = inspectDiscSource(folder.path() / "disc.iso");
    REQUIRE(source.ok());
    CHECK(source.value().kind == DiscSource::Kind::Image);
    CHECK(source.value().fileCount == 126);
    CHECK(source.value().totalBytes > 5 * 1024 * 1024);
    CHECK(source.value().release.state == GameFilesState::UnsupportedVersion);
    CHECK(source.value().release.sha256.size() == 64);
    CHECK_FALSE(std::filesystem::exists(folder.path() / "game"));

    XisoBuilder notAGame;
    notAGame.addFile("readme.txt", "hello");
    folder.write("other.iso", asText(notAGame.build()));
    const auto other = inspectDiscSource(folder.path() / "other.iso");
    REQUIRE_FALSE(other.ok());
    CHECK(other.error().message.find("default.xbe") != std::string::npos);
}

TEST_CASE("A release other than the supported one is not copied") {
    const TempFolder folder;
    folder.write("disc.iso", asText(sampleDisc().build()));
    const auto source = inspectDiscSource(folder.path() / "disc.iso");
    REQUIRE(source.ok());
    const auto result = importDisc(source.value(), folder.path() / "game", {}, {});
    CHECK_FALSE(result.ok());
    CHECK_FALSE(std::filesystem::exists(folder.path() / "game" / "default.xbe"));
}

TEST_CASE("Importing an image copies every file, reports progress, and writes default.xbe last") {
    const TempFolder folder;
    folder.write("disc.iso", asText(sampleDisc().build()));
    folder.write("game/UDATA/save.bin", "my save");
    const DiscSource source = importable(folder.path() / "disc.iso");

    std::vector<ImportProgress> reports;
    bool xbeBeforeOthers = false;
    const auto result =
        importDisc(source, folder.path() / "game",
                   [&](const ImportProgress& progress) {
                       if (progress.item == "default.xbe" && progress.doneFiles + 1 < progress.totalFiles) {
                           xbeBeforeOthers = true;
                       }
                       reports.push_back(progress);
                   },
                   {});
    REQUIRE(result.ok());
    CHECK(result.value());
    CHECK_FALSE(xbeBeforeOthers);
    CHECK(readText(folder.path() / "game" / "levels" / "m01" / "level.bin") == "level one");
    CHECK(std::filesystem::file_size(folder.path() / "game" / "sound" / "music.bin") == 5 * 1024 * 1024 + 7);
    CHECK(readText(folder.path() / "game" / "UDATA" / "save.bin") == "my save");

    REQUIRE_FALSE(reports.empty());
    CHECK(reports.back().stage == ImportProgress::Stage::Done);
    CHECK(reports.back().doneFiles == source.fileCount);
    CHECK(reports.back().doneBytes == source.totalBytes);
    for (std::size_t i = 1; i < reports.size(); ++i) {
        CHECK(reports[i].doneBytes >= reports[i - 1].doneBytes);
    }
}

TEST_CASE("A cancelled import stops, leaves no default.xbe, and resumes without recopying") {
    const TempFolder folder;
    folder.write("disc.iso", asText(sampleDisc().build()));
    const DiscSource source = importable(folder.path() / "disc.iso");

    std::stop_source cancel;
    const auto first = importDisc(
        source, folder.path() / "game",
        [&](const ImportProgress& progress) {
            if (progress.doneFiles >= 10) {
                cancel.request_stop();
            }
        },
        cancel.get_token());
    REQUIRE(first.ok());
    CHECK_FALSE(first.value());
    CHECK_FALSE(std::filesystem::exists(folder.path() / "game" / "default.xbe"));
    CHECK(bytesStillNeeded(source, folder.path() / "game") < source.totalBytes);

    folder.write("game/GlobData.bin", std::string(10000, 'k'));
    const auto second = importDisc(source, folder.path() / "game", {}, {});
    REQUIRE(second.ok());
    CHECK(second.value());
    CHECK(readText(folder.path() / "game" / "GlobData.bin") == std::string(10000, 'k'));
    CHECK(std::filesystem::exists(folder.path() / "game" / "default.xbe"));
    CHECK(bytesStillNeeded(source, folder.path() / "game") == 0);
}

TEST_CASE("A folder of extracted files is copied the same way, without its saves") {
    const TempFolder folder;
    folder.write("extracted/default.xbe", "XBEH pretend executable");
    folder.write("extracted/levels/m01/level.bin", "level one");
    folder.write("extracted/GlobData.bin", "globals");
    folder.write("extracted/sound/music.bin", "music");
    folder.write("extracted/videos/intro.bin", "intro");
    folder.write("extracted/UDATA/someone_elses_save.bin", "not mine");
    const DiscSource source = importable(folder.path() / "extracted");
    CHECK(source.kind == DiscSource::Kind::Folder);
    CHECK(source.fileCount == 5);

    const auto result = importDisc(source, folder.path() / "game", {}, {});
    REQUIRE(result.ok());
    CHECK(readText(folder.path() / "game" / "levels" / "m01" / "level.bin") == "level one");
    CHECK_FALSE(std::filesystem::exists(folder.path() / "game" / "UDATA"));

    const auto noXbe = inspectDiscSource(folder.path() / "extracted" / "levels");
    REQUIRE_FALSE(noXbe.ok());
    CHECK(noXbe.error().message.find("default.xbe") != std::string::npos);
}

TEST_CASE("Disc import rejects missing or mistyped data even for the in-place folder") {
    const TempFolder folder;
    folder.write("disc/default.xbe", "XBEH pretend executable");
    const auto partial = importable(folder.path() / "disc");
    for (const auto destination : {"game", "disc"}) {
        const auto result = importDisc(partial, folder.path() / destination, {}, {});
        REQUIRE_FALSE(result.ok());
        CHECK(result.error().message.find("GlobData.bin") != std::string::npos);
    }
    CHECK_FALSE(std::filesystem::exists(folder.path() / "game"));
    folder.write("disc/GlobData.bin", "globals");
    folder.write("disc/levels", "wrong type");
    const auto wrongType = importDisc(importable(folder.path() / "disc"), folder.path() / "game", {}, {});
    REQUIRE_FALSE(wrongType.ok());
    CHECK(wrongType.error().message.find("levels") != std::string::npos);
}
