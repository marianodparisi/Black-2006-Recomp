#include "core/GameFiles.h"

#include "core/Sha256.h"

#include <system_error>

namespace black::core {

GameFilesStatus inspectGameFiles(const std::filesystem::path& defaultXbe) {
    GameFilesStatus status;
    std::error_code error;
    if (!std::filesystem::is_regular_file(defaultXbe, error)) {
        status.state = GameFilesState::Missing;
        return status;
    }
    Result<Sha256::Digest> digest = sha256File(defaultXbe);
    if (!digest) {
        status.state = GameFilesState::Unreadable;
        status.error = digest.error().message;
        return status;
    }
    status.sha256 = Sha256::toHex(digest.value());
    status.state =
        status.sha256 == kSupportedXbeSha256 ? GameFilesState::Supported : GameFilesState::UnsupportedVersion;
    return status;
}

std::string_view missingDiscData(const std::filesystem::path& gameDirectory) {
    for (const auto& entry : kRequiredDiscData) {
        std::error_code error;
        const auto status = std::filesystem::status(gameDirectory / entry.name, error);
        if (error ||
            !(entry.directory ? std::filesystem::is_directory(status) : std::filesystem::is_regular_file(status))) {
            return entry.name;
        }
    }
    return {};
}

}  // namespace black::core
