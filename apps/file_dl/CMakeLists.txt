cmake_minimum_required(VERSION 2.6.4)
project(CFS_FILE_DL C)

include_directories(fsw/platform_inc)

# add_cfe_app(file_dl fsw/src/file_dl_my.c)
# file_ul_my.c adds upload and relay to the same app.
add_cfe_app(file_dl fsw/src/file_dl_my.c fsw/src/file_ul_my.c)
