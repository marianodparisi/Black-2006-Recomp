#include "core/ProjectLock.h"

#ifdef _WIN32
#include "core/Sha256.h"
#include "core/Text.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <span>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace black::core {
namespace fs = std::filesystem;

ProjectLock::~ProjectLock() {
#ifdef _WIN32
    if (m_mutex != nullptr) {
        ReleaseMutex(m_mutex);
        CloseHandle(m_mutex);
    }
#else
    if (m_file >= 0) {
        close(m_file);
    }
#endif
}

std::unique_ptr<ProjectLock> ProjectLock::acquire(const ProjectLayout& project, Use use) {
    std::unique_ptr<ProjectLock> lock(new ProjectLock());
    const bool settings = use == Use::Settings;
#ifdef _WIN32
    // Preserve the mutex name used by existing game launchers. Settings have an independent mutex.
    std::string root = toUtf8(fs::absolute(project.root()));
    while (!root.empty() && (root.back() == '/' || root.back() == '\\')) {
        root.pop_back();
    }
    std::transform(root.begin(), root.end(), root.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    Sha256 hash;
    hash.update(std::as_bytes(std::span(root)));
    const std::wstring name = (settings ? L"Local\\BlackXboxSettings-" : L"Local\\BlackXboxGame-") +
                              pathFromUtf8(Sha256::toHex(hash.finish())).wstring();
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
    if (mutex == nullptr) {
        return nullptr;
    }
    const DWORD state = WaitForSingleObject(mutex, settings ? INFINITE : 0);
    if (state != WAIT_OBJECT_0 && state != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return nullptr;
    }
    lock->m_mutex = mutex;
#else
    if (settings) {
        // Lock the existing directory inode so reading or rejecting settings creates no files.
        lock->m_file = open(project.root().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } else {
        std::error_code error;
        fs::create_directories(project.localDirectory(), error);
        lock->m_file = open((project.localDirectory() / ".game.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    }
    if (lock->m_file < 0) {
        return nullptr;
    }
    int result;
    do {
        result = flock(lock->m_file, settings ? LOCK_EX : LOCK_EX | LOCK_NB);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        return nullptr;
    }
#endif
    return lock;
}

}  // namespace black::core
