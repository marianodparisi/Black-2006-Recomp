# Unit tests for black_launcher_core. They need no game files, GPU or window.
add_executable(black_launcher_tests
    ChildProcessTests.cpp
    DiscTests.cpp
    ErrorReportTests.cpp
    FormatTests.cpp
    GameArtTests.cpp
    GameBuildTests.cpp
    GameFilesTests.cpp
    GameSessionTests.cpp
    InputBindingsTests.cpp
    LanguageBanksTests.cpp
    ProfileTests.cpp
    ProjectLayoutTests.cpp
    SettingsTests.cpp
    Sha256Tests.cpp
    SteamShortcutTests.cpp
    TestMain.cpp
    TextTests.cpp)
target_include_directories(black_launcher_tests SYSTEM PRIVATE "${BLACK_VENDOR}/doctest")
target_link_libraries(black_launcher_tests PRIVATE black_launcher_core)
black_launcher_warnings(black_launcher_tests)
add_test(NAME black_launcher_tests COMMAND black_launcher_tests)

# Exercise settings and Steam page routing without opening a window.
add_executable(black_launcher_settings_tests
    SettingsFlowTests.cpp SteamFlowTests.cpp TuningSettingsTests.cpp TestMain.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/app/SettingsFlow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/app/SteamFlow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/app/KeyCapture.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/app/TextEntry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/app/Translations.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/../src/app/EmbeddedAssets.cpp")
target_include_directories(black_launcher_settings_tests SYSTEM PRIVATE "${BLACK_VENDOR}/doctest")
target_include_directories(black_launcher_settings_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(black_launcher_settings_tests PRIVATE black_launcher_core black_walnut black_json)
black_launcher_warnings(black_launcher_settings_tests)
add_test(NAME black_launcher_settings_tests COMMAND black_launcher_settings_tests)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME black_launcher_cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py" "$<TARGET_FILE:black_launcher>")
endif()
