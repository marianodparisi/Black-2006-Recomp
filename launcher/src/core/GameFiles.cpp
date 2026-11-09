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
        isSupportedXbeSha256(status.sha256) ? GameFilesState::Supported : GameFilesState::UnsupportedVersion;
    return status;
}

}  // namespace black::core
