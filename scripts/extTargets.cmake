include(ExternalProject)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(miniaudio_h "${CMAKE_INSTALL_PREFIX}/include/miniaudio")
find_library(miniaudio_lib NAMES miniaudio PATHS ${CMAKE_INSTALL_PREFIX}/lib)
if(NOT miniaudio_lib) # installed here once the miniaudio target builds, so one configure is enough
    set(miniaudio_lib "${CMAKE_INSTALL_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}miniaudio${CMAKE_STATIC_LIBRARY_SUFFIX}")
endif()

set(kissfft_dir "${EXTERNAL_PROJ_DIR}/kissfft/src/kissfft")
set(kissfft_src "${kissfft_dir}/kiss_fft.c" "${kissfft_dir}/kiss_fftr.c") # compiled into SDLIB
# find_file(kissfft_header NAMES kiss_fft.h PATHS ${EXTERNAL_PROJ_DIR}/kissfft/src/kissfft ${CMAKE_INSTALL_PREFIX}/include/kissfft)

ExternalProject_Add(miniaudio
    GIT_REPOSITORY "https://github.com/mackron/miniaudio.git"
    GIT_TAG "9634bedb5b5a2ca38c1ee7108a9358a4e233f14d"

    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_INSTALL_PREFIX} -DCMAKE_INSTALL_LIBDIR=lib

    PREFIX ${EXTERNAL_PROJ_DIR}/miniaudio
    INSTALL_DIR ${CMAKE_INSTALL_PREFIX}
    BUILD_BYPRODUCTS ${miniaudio_lib}
)

ExternalProject_Add(kissfft
    GIT_REPOSITORY "https://github.com/mborgerding/kissfft.git"
    GIT_TAG "6398d8a1d0c92486b5ece8a456fd5e6a97ad1f08"

    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_INSTALL_PREFIX}

    PREFIX ${EXTERNAL_PROJ_DIR}/kissfft
    CONFIGURE_COMMAND "" # Manual Configure?
    BUILD_COMMAND "" # Manual Build?
    INSTALL_COMMAND "" # Manual Install?
    INSTALL_DIR ${CMAKE_INSTALL_PREFIX}
    BUILD_BYPRODUCTS ${kissfft_src}
)

# SUPPORT_MIDI needs no external project, plugin/MIDI.c reads and writes MIDI files on its own
# Steev's midilib (https://github.com/MarquisdeGeek/midilib) is left out: it is released under the GPL 2,
# and midifile.c includes <endian.h>, which the Windows toolchains do not provide

# TODO: Check CUDA for cuFFT support?

message(STATUS "miniaudio library is ${miniaudio_lib} and header is ${miniaudio_h}")
message(STATUS "kissfft directory is ${EXTERNAL_PROJ_DIR}/kissfft")
