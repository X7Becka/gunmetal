# Toolchain: zig cc/c++ + REAL ASan/UBSan.
#
# Install the runtimes once with tools/qa/install-asan-runtime.sh, then use
# the `asan` preset (or pass this file as CMAKE_TOOLCHAIN_FILE explicitly).
#
# Set GM_SAN_ROOT to relocate the extracted runtimes.

set(CMAKE_SYSTEM_NAME Linux)

set(GM_SAN_ROOT "$ENV{HOME}/tools/sanitizers" CACHE PATH "Prefix holding usr/lib/x86_64-linux-gnu/libasan.so.8")
set(GM_SAN_LIB "${GM_SAN_ROOT}/usr/lib/x86_64-linux-gnu")

set(GM_ZIG_BIN "$ENV{HOME}/tools/bin" CACHE PATH "Directory containing zigcc/zigcxx wrappers")

set(CMAKE_MAKE_PROGRAM "$ENV{HOME}/tools/bin/ninja" CACHE FILEPATH "" FORCE)

set(CMAKE_C_COMPILER   "${GM_ZIG_BIN}/zigcc"  CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${GM_ZIG_BIN}/zigcxx" CACHE FILEPATH "" FORCE)

# IMPORTANT: two separate flags. A comma list ("address,undefined") makes
# zig's clang silently skip ASan entirely -- instrumentation is dropped and
# the build looks healthy while detecting nothing (this is the bug that made
# the repo's "asan" preset a false gate).
set(GM_SAN_COMPILE_FLAGS "-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -g" CACHE STRING "")

set(CMAKE_C_FLAGS_INIT   "${GM_SAN_COMPILE_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${GM_SAN_COMPILE_FLAGS}")

# Link libasan from the trailing position so the linker can resolve the
# __asan_* references emitted by the objects. CMAKE_*_LINKER_FLAGS would be
# placed *before* the objects/libraries and would not resolve them.
set(CMAKE_C_STANDARD_LIBRARIES_INIT   "-L${GM_SAN_LIB} -Wl,-rpath,${GM_SAN_LIB} -l:libasan.so.8 ${CMAKE_C_STANDARD_LIBRARIES_INIT}")
set(CMAKE_CXX_STANDARD_LIBRARIES_INIT "-L${GM_SAN_LIB} -Wl,-rpath,${GM_SAN_LIB} -l:libasan.so.8 ${CMAKE_CXX_STANDARD_LIBRARIES_INIT}")

# Shared libs (if the project ever builds any) need the same treatment.
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L${GM_SAN_LIB} -Wl,-rpath,${GM_SAN_LIB} -l:libasan.so.8")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-L${GM_SAN_LIB} -Wl,-rpath,${GM_SAN_LIB} -l:libasan.so.8")
