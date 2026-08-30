# Cross-compilation contract for the K230D Linux/Xuantie glibc target.
#
# The compiler location is deployment-local state and must be supplied by the
# caller. Architecture, ABI and ELF interpreter selection are repository build
# policy and therefore live here instead of in per-application scripts.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED ENV{MEISO_K230_LINUX_CC} OR
   "$ENV{MEISO_K230_LINUX_CC}" STREQUAL "")
    message(
        FATAL_ERROR
        "MEISO_K230_LINUX_CC must name the Xuantie Linux C compiler"
    )
endif()

set(
    CMAKE_C_COMPILER
    "$ENV{MEISO_K230_LINUX_CC}"
    CACHE FILEPATH "Xuantie Linux C compiler"
)

set(
    MEISO_K230D_LINUX_ARCH_FLAGS
    "-march=rv64gcv_xtheadc -mabi=lp64d"
    CACHE STRING "K230D Linux compiler architecture and ABI flags"
)
set(
    MEISO_K230D_LINUX_DYNAMIC_LINKER
    "/lib/ld-linux-riscv64-lp64d.so.1"
    CACHE STRING "K230D Linux ELF interpreter"
)

# These cache switches make the target intent visible to the ordinary project
# graph. The application module still validates the platform before exposing
# K230D-only executable targets.
set(MEISO_K230_LINUX_TARGET ON CACHE BOOL "Build for K230D Linux")
set(
    MEISO_BUILD_K230D_LINUX_APPS
    ON
    CACHE BOOL "Build K230D Linux application targets"
)

# Toolchain files can be evaluated more than once during compiler discovery;
# exact assignment keeps repeated evaluation idempotent.
set(CMAKE_C_FLAGS_INIT "${MEISO_K230D_LINUX_ARCH_FLAGS}")
set(
    CMAKE_EXE_LINKER_FLAGS_INIT
    "-Wl,--dynamic-linker=${MEISO_K230D_LINUX_DYNAMIC_LINKER}"
)
set(
    CMAKE_C_FLAGS_RELEASE
    "-O2 -DNDEBUG"
    CACHE STRING "K230D Linux release compiler flags"
)
