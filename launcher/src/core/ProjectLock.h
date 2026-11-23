#pragma once

#include "core/ProjectLayout.h"

#include <memory>

namespace black::core {

class ProjectLock {
public:
    enum class Use { GameSession, Settings };

    /// One game per project (try once), or one settings transaction at a time (wait).
    [[nodiscard]] static std::unique_ptr<ProjectLock> acquire(const ProjectLayout& project, Use use);
    ~ProjectLock();
    ProjectLock(const ProjectLock&) = delete;
    ProjectLock& operator=(const ProjectLock&) = delete;
    ProjectLock(ProjectLock&&) = delete;
    ProjectLock& operator=(ProjectLock&&) = delete;

private:
    ProjectLock() = default;
#ifdef _WIN32
    void* m_mutex = nullptr;
#else
    int m_file = -1;
#endif
};

}  // namespace black::core
