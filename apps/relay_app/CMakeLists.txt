project(CFE_RELAY_APP C)

include_directories(fsw/mission_inc)
include_directories(fsw/platform_inc)

# FILE_DL message IDs and its START command / segment header layouts
include_directories(${file_dl_MISSION_DIR}/fsw/platform_inc)
include_directories(${file_dl_MISSION_DIR}/fsw/src)

# Create the app module
add_cfe_app(relay_app fsw/src/relay_app.c)
