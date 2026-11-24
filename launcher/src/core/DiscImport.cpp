#include "core/DiscImport.h"

#include "core/Format.h"
#include "core/Sha256.h"
#include "core/Text.h"
#include "core/Xdvdfs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <optional>

namespace black::core {
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kXbe = "default.xbe";
constexpr std::uint64_t kFreeSpaceMargin = 64ULL * 1024 * 1024;
constexpr std::size_t kCopyChunk = 4U * 1024U * 1024U;

bool sameName(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

GameFilesStatus releaseOf(std::span<const std::uint8_t> xbe) {
    Sha256 hash;
    hash.update(std::as_bytes(xbe));
    GameFilesStatus status;
    status.sha256 = Sha256::toHex(hash.finish());
    status.state =
        isSupportedXbeSha256(status.sha256) ? GameFilesState::Supported : GameFilesState::UnsupportedVersion;
    return status;
}

void addTotals(DiscSource& source) {
    source.totalBytes = 0;
    source.fileCount = 0;
    for (const DiscSource::File& file : source.files) {
        if (!file.directory) {
            source.totalBytes += file.size;
            ++source.fileCount;
        }
    }
}

Result<DiscSource> inspectImage(const fs::path& location) {
    Result<xdvdfs::Image> image = xdvdfs::Image::open(location);
    if (!image) {
        return image.error();
    }
    Result<std::vector<xdvdfs::Entry>> entries = image.value().list();
    if (!entries) {
        return entries.error();
    }
    DiscSource source;
    source.kind = DiscSource::Kind::Image;
    source.location = location;
    source.partitionOffset = image.value().partitionOffset();
    std::optional<xdvdfs::Entry> xbe;
    for (const xdvdfs::Entry& entry : entries.value()) {
        source.files.push_back({entry.path, entry.size, entry.sector, entry.directory});
        if (!entry.directory && sameName(entry.path, kXbe)) {
            xbe = entry;
        }
    }
    if (!xbe) {
        return Error{"This Xbox disc image has no default.xbe, so it is not a game disc."};
    }
    Result<std::vector<std::uint8_t>> bytes = image.value().read(*xbe);
    if (!bytes) {
        return bytes.error();
    }
    source.release = releaseOf(bytes.value());
    addTotals(source);
    return source;
}

Result<DiscSource> inspectFolder(const fs::path& location) {
    std::error_code error;
    if (!fs::is_regular_file(location / kXbe, error)) {
        return Error{"This folder has no default.xbe. Choose the folder with your disc's files, or its ISO."};
    }
    DiscSource source;
    source.kind = DiscSource::Kind::Folder;
    source.location = location;
    for (auto it = fs::recursive_directory_iterator(location, fs::directory_options::skip_permission_denied, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        const fs::path relative = it->path().lexically_relative(location);
        const std::u8string generic = relative.generic_u8string();
        const std::string path(generic.begin(), generic.end());
        // Saves belong to the game folder being filled, not the disc
        const std::string top = path.substr(0, path.find('/'));
        if (sameName(top, "UDATA") || sameName(top, "TDATA")) {
            if (it->is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const bool directory = it->is_directory(error);
        source.files.push_back({path, directory ? 0 : it->file_size(error), 0, directory});
    }
    if (error) {
        return Error{"Could not read the folder " + toUtf8(location) + ": " + error.message()};
    }
    source.release = inspectGameFiles(location / kXbe);
    addTotals(source);
    return source;
}

bool alreadyInPlace(const DiscSource::File& file, const fs::path& destination) {
    std::error_code error;
    return fs::is_regular_file(destination, error) && fs::file_size(destination, error) == file.size && !error;
}

Result<bool> copyPlainFile(const fs::path& from, const fs::path& to, const std::function<bool(std::uint64_t)>& copied) {
    std::ifstream in(from, std::ios::binary);
    if (!in) {
        return Error{"Could not read " + toUtf8(from)};
    }
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) {
        return Error{"Could not write " + toUtf8(to)};
    }
    std::vector<char> buffer(kCopyChunk);
    std::uint64_t done = 0;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = in.gcount();
        if (count <= 0) {
            break;
        }
        out.write(buffer.data(), count);
        if (!out) {
            return Error{"Could not write " + toUtf8(to) + ". Is the disk full?"};
        }
        done += static_cast<std::uint64_t>(count);
        if (!copied(done)) {
            return false;
        }
    }
    if (in.bad()) {
        return Error{"Could not read " + toUtf8(from)};
    }
    return true;
}

fs::path existingAncestor(fs::path folder) {
    std::error_code error;
    while (!folder.empty() && !fs::exists(folder, error)) {
        const fs::path parent = folder.parent_path();
        if (parent == folder) {
            break;
        }
        folder = parent;
    }
    return folder;
}

}  // namespace

Result<DiscSource> inspectDiscSource(const fs::path& location) {
    std::error_code error;
    if (fs::is_directory(location, error)) {
        return inspectFolder(location);
    }
    if (fs::is_regular_file(location, error)) {
        return inspectImage(location);
    }
    return Error{"Could not find " + toUtf8(location)};
}

std::uint64_t bytesStillNeeded(const DiscSource& source, const fs::path& gameDirectory) {
    std::uint64_t needed = 0;
    for (const DiscSource::File& file : source.files) {
        if (!file.directory && !alreadyInPlace(file, gameDirectory / pathFromUtf8(file.path))) {
            needed += file.size;
        }
    }
    return needed;
}

Result<bool> importDisc(const DiscSource& source, const fs::path& gameDirectory, const ImportProgressCallback& progress,
                        std::stop_token stop) {
    ImportProgress state;
    state.totalBytes = source.totalBytes;
    state.totalFiles = source.fileCount;
    const auto report = [&] {
        if (progress) {
            progress(state);
        }
    };

    std::error_code error;
    if (source.release.state != GameFilesState::Supported) {
        return Error{"This is not the supported Xbox retail release of BLACK, so it was not copied."};
    }
    for (const auto& required : kRequiredDiscData) {
        if (std::ranges::none_of(source.files, [&](const auto& entry) {
                return entry.directory == required.directory && sameName(entry.path, required.name);
            })) {
            return Error{"The disc is missing " + std::string(required.name) +
                         ". Choose the complete disc image or folder."};
        }
    }
    if (source.kind == DiscSource::Kind::Folder && fs::exists(gameDirectory, error) &&
        fs::equivalent(source.location, gameDirectory, error)) {
        if (const auto missing = missingDiscData(gameDirectory); !missing.empty()) {
            return Error{"The game folder is missing " + std::string(missing)};
        }
        state.stage = ImportProgress::Stage::Done;
        state.doneBytes = state.totalBytes;
        state.doneFiles = state.totalFiles;
        report();
        return true;  // the files are already where the game reads them
    }

    state.stage = ImportProgress::Stage::Checking;
    report();
    const std::uint64_t needed = bytesStillNeeded(source, gameDirectory);
    const fs::space_info space = fs::space(existingAncestor(gameDirectory), error);
    if (!error && space.available < needed + kFreeSpaceMargin) {
        return Error{"Not enough free space: the game needs " + formatBytes(needed + kFreeSpaceMargin) +
                     " and the drive has " + formatBytes(space.available) + " free."};
    }

    std::optional<xdvdfs::Image> image;
    if (source.kind == DiscSource::Kind::Image) {
        const std::array<std::uint64_t, 1> partition = {source.partitionOffset};
        Result<xdvdfs::Image> opened = xdvdfs::Image::open(source.location, partition);
        if (!opened) {
            return opened.error();
        }
        image.emplace(std::move(opened).value());
    }

    // default.xbe goes last: until it is there, the game folder does not count as ready
    std::vector<const DiscSource::File*> order;
    for (const DiscSource::File& file : source.files) {
        order.push_back(&file);
    }
    std::stable_partition(order.begin(), order.end(),
                          [](const DiscSource::File* file) { return !sameName(file->path, kXbe); });

    fs::create_directories(gameDirectory, error);
    if (error) {
        return Error{"Could not create " + toUtf8(gameDirectory) + ": " + error.message()};
    }
    state.stage = ImportProgress::Stage::Copying;
    for (const DiscSource::File* file : order) {
        const fs::path destination = gameDirectory / pathFromUtf8(file->path);
        if (file->directory) {
            fs::create_directories(destination, error);
            if (error) {
                return Error{"Could not create " + toUtf8(destination) + ": " + error.message()};
            }
            continue;
        }
        state.item = file->path;
        report();
        if (stop.stop_requested()) {
            return false;
        }
        const std::uint64_t before = state.doneBytes;
        if (!alreadyInPlace(*file, destination)) {
            fs::create_directories(destination.parent_path(), error);
            const auto copied = [&](std::uint64_t bytes) {
                state.doneBytes = before + bytes;
                report();
                return !stop.stop_requested();
            };
            Result<bool> result =
                image ? image->copyTo(xdvdfs::Entry{file->path, file->sector, file->size, false}, destination, copied)
                      : copyPlainFile(source.location / pathFromUtf8(file->path), destination, copied);
            if (!result) {
                return result.error();
            }
            if (!result.value()) {
                return false;  // cancelled; the partial file is redone next time (its size differs)
            }
        }
        state.doneBytes = before + file->size;
        ++state.doneFiles;
        report();
    }

    state.stage = ImportProgress::Stage::Finishing;
    state.item.clear();
    report();
    if (const auto missing = missingDiscData(gameDirectory); !missing.empty()) {
        return Error{"The game folder is missing " + std::string(missing)};
    }
    const GameFilesStatus copied = inspectGameFiles(gameDirectory / kXbe);
    if (copied.sha256 != source.release.sha256) {
        return Error{
            "The copied default.xbe does not match the disc. Try again; if it happens again, the drive may "
            "be failing."};
    }
    state.stage = ImportProgress::Stage::Done;
    report();
    return true;
}

}  // namespace black::core
