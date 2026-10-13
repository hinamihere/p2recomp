cmake_minimum_required(VERSION 3.20)
project(p2recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(PSPRECOMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(framework)

# Recompiled guest code, produced by tools/regen.sh (not committed: derived from the game).
set(P2_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH "Directory holding psp_recomp output")
file(GLOB P2_GENERATED_SOURCES CONFIGURE_DEPENDS "${P2_GENERATED_DIR}/*.cpp")
if(NOT P2_GENERATED_SOURCES)
    message(FATAL_ERROR "No generated sources in ${P2_GENERATED_DIR}; run tools/regen.sh first")
endif()
add_library(p2_generated STATIC ${P2_GENERATED_SOURCES})
target_include_directories(p2_generated PRIVATE "${P2_GENERATED_DIR}")
target_link_libraries(p2_generated PUBLIC psprecomp_core)
set(P2_GENERATED_OPT "-O2" CACHE STRING "Optimization flags for generated units")
target_compile_options(p2_generated PRIVATE ${P2_GENERATED_OPT} -w -fno-strict-aliasing)

set(SDL2_ROOT "${CMAKE_SOURCE_DIR}/tools/third_party/sdl2/SDL2-2.32.10/x86_64-w64-mingw32" CACHE PATH "SDL2 MinGW dev root")

# ATRAC3/ATRAC3+ decoder from PPSSPP's ext/at3_standalone (FFmpeg-derived, LGPL 2.1+).
file(GLOB AT3_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/game/third_party/at3_standalone/*.cpp")
add_library(at3_standalone STATIC ${AT3_SOURCES})
target_include_directories(at3_standalone PUBLIC "${CMAKE_SOURCE_DIR}/game/third_party/at3_standalone")
target_compile_options(at3_standalone PRIVATE -O2 -w)

file(GLOB P2_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/game/src/*.cpp")
add_executable(p2 ${P2_SOURCES})
target_include_directories(p2 PRIVATE "${SDL2_ROOT}/include")
target_link_directories(p2 PRIVATE "${SDL2_ROOT}/lib")
target_link_libraries(p2 PRIVATE p2_generated psprecomp_core at3_standalone SDL2)
target_compile_options(p2 PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_options(p2 PRIVATE -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
add_custom_command(TARGET p2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDL2_ROOT}/bin/SDL2.dll" "$<TARGET_FILE_DIR:p2>")
