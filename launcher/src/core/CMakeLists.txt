# The launcher's logic, with no UI or graphics dependencies, so it can be unit-tested on its own.
add_library(black_launcher_core STATIC
    ChildProcess.cpp
    DiscImport.cpp
    ErrorReport.cpp
    Files.cpp
    Format.cpp
    GameArt.cpp
    GameFiles.cpp
    GameBuild.cpp
    GameSession.cpp
    InputBindings.cpp
    LanguageBanks.cpp
    Process.cpp
    Profile.cpp
    ProjectLayout.cpp
    ProjectLock.cpp
    Settings.cpp
    Sha1.cpp
    Sha256.cpp
    SteamShortcut.cpp
    Text.cpp
    Toolchain.cpp
    TraceLog.cpp
    Xdvdfs.cpp)
target_include_directories(black_launcher_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(black_launcher_core PUBLIC cxx_std_20)
target_link_libraries(black_launcher_core PRIVATE black_json)
black_launcher_warnings(black_launcher_core)
if(WIN32)
    target_compile_definitions(black_launcher_core PRIVATE UNICODE _UNICODE)
    target_link_libraries(black_launcher_core PRIVATE shell32 advapi32)
endif()
