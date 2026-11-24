#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace black::core {

/// SHA-256 of the NTSC retail Xbox default.xbe.
inline constexpr std::string_view kSupportedXbeSha256 =
    "DF2739C372D254A90AEECCF5971D12097F22D89FBB90DB021FDA96DD4DEB68F6";

/// SHA-256 of the European (PAL) retail default.xbe. scripts/black_release.py describes how its layout differs. The
/// PAL disc has no MainUS.bin: the game picks its own MainUK/Fr/Ge/It/Sp bank from the console language.
inline constexpr std::string_view kPalXbeSha256 = "67F35121CCF622E7A293C402219596FE6456CA4C3D27E6F8139E449264DA30F0";

[[nodiscard]] constexpr bool isSupportedXbeSha256(std::string_view sha256) {
    return sha256 == kSupportedXbeSha256 || sha256 == kPalXbeSha256;
}

enum class GameFilesState {
    Missing,
    Unreadable,
    UnsupportedVersion,
    Supported,
};

struct GameFilesStatus {
    GameFilesState state = GameFilesState::Missing;
    std::string sha256;
    std::string error;
};

[[nodiscard]] GameFilesStatus inspectGameFiles(const std::filesystem::path& defaultXbe);

struct RequiredDiscEntry {
    std::string_view name;
    bool directory;
};
inline constexpr std::array<RequiredDiscEntry, 4> kRequiredDiscData = {
    {{"GlobData.bin", false}, {"levels", true}, {"sound", true}, {"videos", true}}};
/// The first missing data entry, or an empty view if the installed layout is ready.
[[nodiscard]] std::string_view missingDiscData(const std::filesystem::path& gameDirectory);

}  // namespace black::core
