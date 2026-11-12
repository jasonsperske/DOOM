cmake_minimum_required(VERSION 3.22)
project(doom C)

# The original 1997 engine, built unchanged apart from portability fixes.
set(DOOM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../linuxdoom-1.10)

file(GLOB ENGINE_SOURCES ${DOOM_SRC}/*.c)
# Linux/X11 platform layer: replaced by the Android files below.
list(REMOVE_ITEM ENGINE_SOURCES
    ${DOOM_SRC}/i_main.c
    ${DOOM_SRC}/i_net.c
    ${DOOM_SRC}/i_sound.c
    ${DOOM_SRC}/i_system.c
    ${DOOM_SRC}/i_video.c)

add_library(doom SHARED
    ${ENGINE_SOURCES}
    android_main.c
    i_music_android.c
    i_net_android.c
    i_sound_android.c
    i_system_android.c
    i_video_android.c
    ride.c)

# The Android backends are modern C; only the engine needs gnu89.
set_source_files_properties(
    android_main.c i_music_android.c i_net_android.c i_sound_android.c i_system_android.c i_video_android.c ride.c
    PROPERTIES COMPILE_OPTIONS "-std=gnu11")

target_include_directories(doom PRIVATE ${DOOM_SRC} ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(doom PRIVATE NORMALUNIX)

target_compile_options(doom PRIVATE
    -std=gnu89
    # ARM's plain char is unsigned; DOOM's ticcmd_t movement fields need it signed.
    -fsigned-char
    -fno-strict-aliasing
    # Headers define globals without extern.
    -fcommon
    -O2
    # 1997 C: implicit declarations, K&R prototypes, loose pointer types.
    -Wno-implicit-function-declaration
    -Wno-implicit-int
    -Wno-int-conversion
    -Wno-incompatible-pointer-types
    -Wno-incompatible-function-pointer-types
    -Wno-pointer-sign
    -Wno-deprecated-non-prototype
    -Wno-unused-value
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-const-variable
    -Wno-dangling-else
    -Wno-parentheses
    -Wno-format
    -Wno-switch
    -Wno-missing-braces
    -Wno-tautological-pointer-compare
    -Wno-void-pointer-to-int-cast
    -Wno-pointer-to-int-cast
    -Wno-int-to-pointer-cast)

target_link_libraries(doom android log OpenSLES m)
