# Cross-compile the Windows x64 build from macOS with clang-cl, lld-link and
# the MSVC CRT + Windows SDK that xwin splatted under ./xwin.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(LLVM_BIN /opt/homebrew/opt/llvm/bin)
set(CMAKE_C_COMPILER ${LLVM_BIN}/clang-cl)
set(CMAKE_CXX_COMPILER ${LLVM_BIN}/clang-cl)
set(CMAKE_LINKER /opt/homebrew/opt/lld/bin/lld-link)
set(CMAKE_AR ${LLVM_BIN}/llvm-lib)
set(CMAKE_RC_COMPILER ${LLVM_BIN}/llvm-rc)
set(CMAKE_MT ${LLVM_BIN}/llvm-mt)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# XWIN: the directory produced by `xwin splat` (env XWIN_DIR, default ~/black/toolchain/xwin).
if(DEFINED ENV{XWIN_DIR})
    set(XWIN $ENV{XWIN_DIR})
else()
    set(XWIN $ENV{HOME}/black/toolchain/xwin)
endif()
set(XWIN_INCLUDES
    "/imsvc${XWIN}/crt/include"
    "/imsvc${XWIN}/sdk/include/ucrt"
    "/imsvc${XWIN}/sdk/include/um"
    "/imsvc${XWIN}/sdk/include/shared"
    "/imsvc${XWIN}/sdk/include/winrt")
string(JOIN " " XWIN_INCLUDE_FLAGS ${XWIN_INCLUDES})
set(CMAKE_C_FLAGS_INIT "--target=x86_64-pc-windows-msvc -fms-compatibility-version=19.40 -Wno-error=implicit-function-declaration ${XWIN_INCLUDE_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")
set(CMAKE_RC_FLAGS_INIT "-I${XWIN}/sdk/include/um -I${XWIN}/sdk/include/shared")

set(XWIN_LIBPATHS
    "/libpath:${XWIN}/crt/lib/x86_64"
    "/libpath:${XWIN}/sdk/lib/um/x86_64"
    "/libpath:${XWIN}/sdk/lib/ucrt/x86_64")
string(JOIN " " XWIN_LINK_FLAGS ${XWIN_LIBPATHS})
set(CMAKE_EXE_LINKER_FLAGS_INIT "${XWIN_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${XWIN_LINK_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${XWIN_LINK_FLAGS}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
