project(CFE_MSN_APP C)

include_directories(fsw/mission_inc)
include_directories(fsw/platform_inc)

# Create the app module
add_cfe_app(msn_app fsw/src/msn_app.c)
