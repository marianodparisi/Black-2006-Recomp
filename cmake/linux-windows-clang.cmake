# Cross-compile Windows x64 with native Linux LLVM and xwin's downloaded SDK.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
# xwin intentionally excludes Microsoft's non-redistributable debug CRT.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

if(NOT DEFINED BLACK_XWIN_ROOT)
    set(BLACK_XWIN_ROOT "$ENV{BLACK_XWIN_ROOT}")
endif()
if(NOT EXISTS "${BLACK_XWIN_ROOT}/crt/include" OR NOT EXISTS "${BLACK_XWIN_ROOT}/sdk/include/um")
    message(FATAL_ERROR "Set BLACK_XWIN_ROOT to the xwin SDK created by scripts/setup-linux.sh.")
endif()
file(TO_CMAKE_PATH "${BLACK_XWIN_ROOT}" BLACK_XWIN_ROOT)
find_program(BLACK_CLANG_CL NAMES clang-cl REQUIRED)
find_program(BLACK_LLD_LINK NAMES lld-link REQUIRED)
find_program(BLACK_LLVM_LIB NAMES llvm-lib REQUIRED)
find_program(BLACK_LLVM_RC NAMES llvm-rc REQUIRED)
find_program(BLACK_LLVM_MT NAMES llvm-mt REQUIRED)
set(CMAKE_C_COMPILER "${BLACK_CLANG_CL}")
set(CMAKE_CXX_COMPILER "${BLACK_CLANG_CL}")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_LINKER "${BLACK_LLD_LINK}")
set(CMAKE_AR "${BLACK_LLVM_LIB}")
set(CMAKE_RC_COMPILER "${BLACK_LLVM_RC}")
set(CMAKE_MT "${BLACK_LLVM_MT}")

set(_black_includes
    "${BLACK_XWIN_ROOT}/crt/include"
    "${BLACK_XWIN_ROOT}/sdk/include/ucrt"
    "${BLACK_XWIN_ROOT}/sdk/include/shared"
    "${BLACK_XWIN_ROOT}/sdk/include/um"
    "${BLACK_XWIN_ROOT}/sdk/include/winrt")
# Guest RAM is intentionally viewed through different integer/float types;
# keep MSVC's aliasing behavior when Clang optimizes those translated accesses.
set(_black_compile_flags " /clang:-fno-strict-aliasing")
foreach(_black_include IN LISTS _black_includes)
    string(APPEND _black_compile_flags " /imsvc\"${_black_include}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_black_compile_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_black_compile_flags}")
set(CMAKE_RC_FLAGS_INIT "-I\"${BLACK_XWIN_ROOT}/sdk/include/um\" -I\"${BLACK_XWIN_ROOT}/sdk/include/shared\"")
set(_black_link_flags "")
foreach(_black_lib IN ITEMS crt/lib/x86_64 sdk/lib/ucrt/x86_64 sdk/lib/um/x86_64)
    string(APPEND _black_link_flags " /libpath:\"${BLACK_XWIN_ROOT}/${_black_lib}\"")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_black_link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_black_link_flags}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES BLACK_XWIN_ROOT)
# Build tools and Python always run on Linux; SDK discovery is explicit above.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
