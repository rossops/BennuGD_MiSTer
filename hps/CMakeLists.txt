cmake_minimum_required(VERSION 3.24)
project(bennugd_mister C)
set(CMAKE_C_STANDARD 11)

# BennuGD_libretro builds its core as a SHARED library whenever the platform
# allows it. We want one self-contained executable, so tell CMake shared
# libraries are unsupported before the submodule is configured: its
# libretro/CMakeLists.txt then picks STATIC for bennugd_libretro.
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS FALSE)
# The core stamps its version from git; with us as the root project that
# would read this repo, so point it at the submodule as upstream intends.
set(GIT_WORKING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/BennuGD_libretro")
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
target_compile_options(bennugd PRIVATE -Wall -Wextra)
set_target_properties(bennugd PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_link_libraries(bennugd PRIVATE bennugd_libretro m pthread dl)
