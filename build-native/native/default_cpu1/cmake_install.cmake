# Install script for directory: /home/heeseung/rtems-cfs-demo/cFS/cfe

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/exe")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/cpu1/cf" TYPE FILE RENAME "cfe_es_startup.scr" FILES "/home/heeseung/rtems-cfs-demo/cFS/bbb_defs/cpu1_cfe_es_startup.scr")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/osal/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/core_api/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/core_private/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/es/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/evs/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/fs/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/sb/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/tbl/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/time/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/psp/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/msg/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/sbr/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/resourceid/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/config/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/apps/ci_lab/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/apps/to_lab/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/apps/sch_lab/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/apps/sample_app/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/apps/sample_lib/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/apps/blinky/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/apps/cfe_assert/cmake_install.cmake")
  include("/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/cpu1/cmake_install.cmake")

endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/home/heeseung/rtems-cfs-demo/cFS/build-native/native/default_cpu1/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
