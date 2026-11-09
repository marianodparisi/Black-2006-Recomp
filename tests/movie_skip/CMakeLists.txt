cmake_minimum_required(VERSION 3.20)
project(movie_skip_test LANGUAGES C)
if(NOT WIN32)
    message(FATAL_ERROR "These native movie input and overlay regressions require Windows.")
endif()
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/xboxrecomp"
    CACHE PATH "XboxRecomp submodule (third_party/xboxrecomp)")
set(BLACK_MANUAL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/recomp_manual.c"
    CACHE FILEPATH "Production authored movie wrapper source")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${BLACK_MANUAL_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_wrapper.py")
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/extract_wrapper.py"
    --source "${BLACK_MANUAL_SOURCE}" --output-dir "${CMAKE_CURRENT_BINARY_DIR}/generated"
    RESULT_VARIABLE extraction_result)
if(NOT extraction_result EQUAL 0)
    message(FATAL_ERROR "Could not derive the movie wrapper from the production source")
endif()

add_executable(movie-input-test test_input.c
    "${XBOXRECOMP_DIR}/src/video/pc_movie_skip.c"
    "${XBOXRECOMP_DIR}/src/input/pc_input.c")
target_compile_definitions(movie-input-test PRIVATE PC_MOVIE_SKIP_TEST)
target_link_libraries(movie-input-test PRIVATE user32)

# Use the real subtitle parser/session and input/control implementations with
# the page/hint renderer. Video-settings services remain stubbed; no Controls
# page or controls profile load is requested.
add_executable(movie-overlay-test test_overlay.c
    "${XBOXRECOMP_DIR}/src/video/pc_menu.c"
    "${XBOXRECOMP_DIR}/src/video/pc_speech_subtitle.c"
    "${XBOXRECOMP_DIR}/src/video/pc_photo.c"
    "${XBOXRECOMP_DIR}/src/video/pc_movie_skip.c"
    "${XBOXRECOMP_DIR}/src/input/pc_input.c"
    "${XBOXRECOMP_DIR}/src/input/pc_controls.c")
target_compile_definitions(movie-overlay-test PRIVATE PC_MOVIE_SKIP_TEST)
target_link_libraries(movie-overlay-test PRIVATE user32 gdi32 shell32 ole32)

# The real gameplay caption session on a test clock (PC_SPEECH_SUBTITLE_TEST).
add_executable(movie-speech-caption-test test_speech_caption.c "${XBOXRECOMP_DIR}/src/video/pc_speech_subtitle.c")
target_compile_definitions(movie-speech-caption-test PRIVATE PC_SPEECH_SUBTITLE_TEST)

add_executable(movie-wrapper-test test_wrapper.c)
target_include_directories(movie-wrapper-test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_executable(movie-prompt-localization-test test_prompt_localization.c)
add_executable(frontend-string-fallback-test test_frontend_string_fallback.c)
add_executable(movie-picture-test test_picture.c)

foreach(target movie-input-test movie-overlay-test movie-speech-caption-test movie-wrapper-test movie-prompt-localization-test frontend-string-fallback-test movie-picture-test)
    target_include_directories(${target} PRIVATE
        "${XBOXRECOMP_DIR}/src/video" "${XBOXRECOMP_DIR}/src/input")
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    set_property(TARGET ${target} PROPERTY C_STANDARD 11)
    set_property(TARGET ${target} PROPERTY C_STANDARD_REQUIRED ON)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -UNDEBUG)
    endif()
endforeach()

enable_testing()
add_test(NAME movie-input COMMAND movie-input-test)
add_test(NAME movie-overlay COMMAND movie-overlay-test)
add_test(NAME movie-speech-caption COMMAND movie-speech-caption-test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME movie-wrapper COMMAND movie-wrapper-test)
add_test(NAME movie-prompt-localization COMMAND movie-prompt-localization-test)
add_test(NAME frontend-string-fallback COMMAND frontend-string-fallback-test)
add_test(NAME movie-picture COMMAND movie-picture-test)
add_test(NAME movie-subtitle-catalog
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/validate_subtitle_catalog.py"
        --root "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set_tests_properties(movie-input movie-overlay movie-speech-caption movie-wrapper movie-prompt-localization frontend-string-fallback movie-subtitle-catalog PROPERTIES TIMEOUT 15)
