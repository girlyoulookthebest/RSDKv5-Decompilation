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

target_compile_options(RetroEngine PRIVATE -O3 -fpermissive)
target_compile_options(${GAME_NAME} PRIVATE -O3 -fpermissive) 



set(SHARED_DEFINES
    SCREEN_XMAX=512 SCREEN_COUNT=1
)
target_compile_definitions(RetroEngine PRIVATE ${SHARED_DEFINES})
target_compile_definitions(${GAME_NAME} PRIVATE ${SHARED_DEFINES})
target_compile_definitions(RetroEngine PRIVATE RETRO_DISABLE_LOG=1)

target_link_libraries(RetroEngine pspdebug pspfpu pspgu pspdisplay pspge pspctrl m)

set(PLATFORM PSP)
create_pbp_file(TARGET RetroEngine
	TITLE "${CMAKE_PROJECT_NAME}"
    ICON_PATH ../../../${RSDK_PATH}/psp/ICON0.png
    BACKGROUND_PATH ../../../${RSDK_PATH}/psp/PIC1.png)