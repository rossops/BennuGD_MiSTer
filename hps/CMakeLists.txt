cmake_minimum_required(VERSION 3.24)
project(bennugd_mister C)
set(CMAKE_C_STANDARD 11)

# BennuGD_libretro builds its core as a SHARED library whenever the platform
# allows it. We want one self-contained executable, so tell CMake shared
# libraries are unsupported before the submodule is configured: its
# libretro/CMakeLists.txt then picks STATIC for bennugd_libretro.
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS FALSE)
# zlib-ng's NEON adler32 issues a VLD1 with a 256-bit alignment qualifier on
# a buffer that is only 16-byte aligned: SIGBUS (alignment fault) when SorR
# inflates the next level's data. The C paths are fast enough here.
set(WITH_NEON OFF CACHE BOOL "zlib-ng NEON: off, see the comment above" FORCE)
# The core stamps its version from git; with us as the root project that
# would read this repo, so point it at the submodule as upstream intends.
set(GIT_WORKING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/BennuGD_libretro")
# zig cc insists on position independent code for this target, so globals
# would be read through the GOT (three dependent loads each) in the
# interpreter loop and the blitters. Hidden visibility lets the compiler
# address every global defined in the same file pc-relative; the hot
# externs carry the attribute themselves (BGDRTM_HOT_GLOBAL in bgdrtm.h).
add_compile_options(-fvisibility=hidden)
add_subdirectory(BennuGD_libretro EXCLUDE_FROM_ALL)

# Upstream builds everything -fPIC for the shared core. We link it into an
# executable, and on 32-bit ARM PIC taxes every global access in the
# interpreter loop, so switch it off for every target in the submodule.
function(bennugd_no_pic dir)
    get_property(tgts DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(t ${tgts})
        get_target_property(type "${t}" TYPE)
        if(type STREQUAL "STATIC_LIBRARY" OR type STREQUAL "OBJECT_LIBRARY" OR type STREQUAL "EXECUTABLE")
            set_target_properties("${t}" PROPERTIES POSITION_INDEPENDENT_CODE OFF)
        endif()
    endforeach()
    get_property(subs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(d ${subs})
        bennugd_no_pic("${d}")
    endforeach()
endfunction()
bennugd_no_pic("${CMAKE_CURRENT_SOURCE_DIR}/BennuGD_libretro")

add_executable(bennugd
    frontend/main.c
    frontend/video.c
    frontend/audio.c
    frontend/input.c
    frontend/launcher.c
    frontend/prof.c
)
target_include_directories(bennugd PRIVATE BennuGD_libretro/libretro BennuGD_libretro/libretro/sdl-libretro/include)

# PGO=gen in build.sh: the LLVM profile runtime (see pgo/README.md)
option(PGO_GEN "link the vendored LLVM profile runtime for an instrumented build" OFF)
if(PGO_GEN)
    # compiled into the executable itself: as a static library nothing would
    # reference it (zig's compiler-rt already defines a dummy runtime symbol)
    file(GLOB PGO_RT_SRC pgo/profile/*.c)
    list(APPEND PGO_RT_SRC pgo/pgo_runtime.c)
    set_source_files_properties(${PGO_RT_SRC} PROPERTIES
        COMPILE_OPTIONS "-fno-profile-instr-generate;-w"
        COMPILE_DEFINITIONS "COMPILER_RT_HAS_ATOMICS=1;COMPILER_RT_HAS_FCNTL_LCK=1;COMPILER_RT_HAS_UNAME=1"
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/pgo/profile")
    target_sources(bennugd PRIVATE ${PGO_RT_SRC})
endif()
target_compile_options(bennugd PRIVATE -Wall -Wextra)
set_target_properties(bennugd PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_link_libraries(bennugd PRIVATE bennugd_libretro m pthread dl)
