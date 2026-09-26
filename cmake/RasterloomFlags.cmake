# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compile flags (BUILD-SPEC <stack>, non-negotiable): -O2 -march=x86-64-v2 -fno-fast-math
# -ffp-contract=off. Never -march=native, never a global -mavx2: the build machine's CPU is not
# the user's, and the failure mode is SIGILL at launch. -ffp-contract=off forbids FMA contraction,
# which docs/math/00-conventions.md C1 requires for byte-exact agreement with the reference.

# Optimisation level: -O2 for every optimised configuration (CMake's Release default is -O3).
set(CMAKE_C_FLAGS_RELEASE            "-O2 -DNDEBUG"    CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE          "-O2 -DNDEBUG"    CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO     "-O2 -g -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO   "-O2 -g -DNDEBUG" CACHE STRING "" FORCE)

# Applied to every target compiled in this project (no third-party sources are compiled here).
add_compile_options(
    -march=x86-64-v2
    -fno-fast-math
    -ffp-contract=off
)

# No build-machine paths in shipped binaries (the public tree is scrubbed of local paths): __FILE__,
# assert() messages and debug info name files relative to the checkout / build tree, and the
# build-tree RUNPATH (which finds the ADS library before install) is $ORIGIN-relative. No trailing
# slash, so DW_AT_comp_dir (the directory itself) is mapped too; the binary-dir map comes last so
# that it wins for a build tree inside the checkout.
add_compile_options(
    "-ffile-prefix-map=${PROJECT_SOURCE_DIR}=."
    "-ffile-prefix-map=${PROJECT_BINARY_DIR}=./build"
)
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)

# Warnings for our own code. rl_warnings(<target>) is called per target so that a future vendored
# dependency does not inherit them.
function(rl_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(RL_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()
