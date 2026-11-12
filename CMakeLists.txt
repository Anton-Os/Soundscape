cmake_minimum_required(VERSION 3.7...3.10)
project(Soundscape DESCRIPTION "Soundscape Audio Library" LANGUAGES C)

set (CMAKE_C_STANDARD 99)
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/INSTALL" CACHE PATH "Install Path" FORCE)
    message("Install Prefix ${CMAKE_INSTALL_PREFIX}")
endif()

set(ASSETS_DIR "${CMAKE_SOURCE_DIR}/assets/" )

set(SUPPORT_MIDI FALSE CACHE BOOL "Include MIDI read and writing support")

# ------------- Libraries and Plugins ------------- #

set(EXTERNAL_PROJ_DIR "${CMAKE_BINARY_DIR}/PROJECTS")
include("scripts/extTargets.cmake") # Builds external dependencies

set(SD_ENABLE_MIDI ${SUPPORT_MIDI})
configure_file("${CMAKE_SOURCE_DIR}/support/support_def.h.in" "${CMAKE_SOURCE_DIR}/support/support_def.h")

include("scripts/internalTargets.cmake") # Builds SDLIB and MIDILIB

# ------------- Examples and Installation ------------- #

include("scripts/demoTargets.cmake") # Builds Writer and Listener
include("scripts/installTargets.cmake") # Installs SDLIB, MIDILIB and headers
