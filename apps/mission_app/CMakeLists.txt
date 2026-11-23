project(CFE_MISSION_APP C)

include_directories(fsw/mission_inc)
include_directories(fsw/platform_inc)

# Create the app module
add_cfe_app(mission_app fsw/src/mission_app.c)
