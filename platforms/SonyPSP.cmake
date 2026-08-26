find_package(PkgConfig REQUIRED)

add_executable(RetroEngine ${RETRO_FILES})


include(FindPkgConfig)

if(NOT GAME_STATIC)
    message(FATAL_ERROR "GAME_STATIC must be on")
endif()

set(RETRO_MOD_LOADER OFF CACHE BOOL "Disable the mod loader" FORCE)
if(RETRO_AUDIO)
pkg_check_modules(OGG ogg)

if(NOT OGG_FOUND)
    set(COMPILE_OGG TRUE)
    message(NOTICE "libogg not found, attempting to build from source")
else()
    message("found libogg")
    target_link_libraries(RetroEngine ${OGG_STATIC_LIBRARIES})
    target_link_options(RetroEngine PRIVATE ${OGG_STATIC_LDLIBS_OTHER})
    target_compile_options(RetroEngine PRIVATE ${OGG_STATIC_CFLAGS})
endif()

pkg_check_modules(THEORA theora theoradec)

if(NOT THEORA_FOUND)
    message("could not find libtheora, attempting to build manually")
    set(COMPILE_THEORA TRUE)
else()
    message("found libtheora")
    target_link_libraries(RetroEngine ${THEORA_STATIC_LIBRARIES})
    target_link_options(RetroEngine PRIVATE ${THEORA_STATIC_LDLIBS_OTHER})
    target_compile_options(RetroEngine PRIVATE ${THEORA_STATIC_CFLAGS})
endif()
endif()

target_compile_options(RetroEngine PRIVATE -O3 -fpermissive -g)
target_compile_options(${GAME_NAME} PRIVATE -O3 -fpermissive -g)

# create_pbp_file (below) strips the ELF in place, which leaves nothing for
# psp-addr2line to resolve a crash PC against. Stash an unstripped copy first
# -- POST_BUILD commands run in the order they're added, so this lands before
# the strip. -g costs nothing at runtime; it only adds debug sections that the
# strip removes from the shipped binary anyway.
add_custom_command(TARGET RetroEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:RetroEngine> $<TARGET_FILE:RetroEngine>.sym
    COMMENT "Saving unstripped ELF for crash symbolication")



set(SHARED_DEFINES
    SCREEN_XMAX=512 SCREEN_COUNT=1
)
target_compile_definitions(RetroEngine PRIVATE ${SHARED_DEFINES})
target_compile_definitions(${GAME_NAME} PRIVATE ${SHARED_DEFINES})
target_compile_definitions(RetroEngine PRIVATE RETRO_DISABLE_LOG=1)

target_compile_definitions(RetroEngine PRIVATE RETRO_AUDIODEVICE_PSP=1)

# pspdmac: sceDmacMemcpy, used to move the finished frame from the
# rasterizer's main-RAM surface into VRAM (see CopyFrameBuffer).
target_link_libraries(RetroEngine pspdebug pspfpu pspgu pspdisplay pspge pspctrl pspaudiolib pspaudio psppower pspdmac m)

set(PLATFORM PSP)
create_pbp_file(TARGET RetroEngine
	TITLE "${CMAKE_PROJECT_NAME}"
    ICON_PATH ../../../${RSDK_PATH}/psp/ICON0.png
    BACKGROUND_PATH ../../../${RSDK_PATH}/psp/PIC1.png
    MUSIC_PATH ../../../${RSDK_PATH}/psp/SND0.at3
    MEMSIZE 1)