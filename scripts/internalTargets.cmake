# -------------- Core Library ------------- #

list(APPEND core_src
    core/Processing.c
    core/Banding.c
    core/Streaming.c
)

set_source_files_properties(${kissfft_src} PROPERTIES GENERATED TRUE) # downloaded by the kissfft target

add_library(SDLIB # SHARED # Core Library
    ${core_src}
    ${kissfft_src}
)

target_include_directories(SDLIB PRIVATE "${CMAKE_INSTALL_PREFIX}/include" ${miniaudio_h})
target_include_directories(SDLIB PRIVATE ${EXTERNAL_PROJ_DIR}/kissfft/src) # for header only build
target_include_directories(SDLIB PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core ${CMAKE_CURRENT_SOURCE_DIR}/support>
    $<BUILD_INTERFACE:${GENERATED_DIR}>
    $<INSTALL_INTERFACE:include/Soundscape>
)
target_link_libraries(SDLIB PUBLIC ${miniaudio_lib})
set_target_properties(SDLIB PROPERTIES C_STANDARD 99 WINDOWS_EXPORT_ALL_SYMBOLS ON)
set_target_properties(SDLIB PROPERTIES FOLDER "Libraries")
add_dependencies(SDLIB miniaudio kissfft) # externals build first

if(MSVC)
    target_compile_definitions(SDLIB PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    target_link_libraries(SDLIB PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
endif()
if(UNIX) # Unix Specific
    find_package(Threads REQUIRED)
    target_link_libraries(SDLIB PUBLIC ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS} m)
endif()

# -------------- MIDI Plugin ------------- #

if(SUPPORT_MIDI)
    add_library(MIDILIB plugin/MIDI.c) # MIDI Library

    target_include_directories(MIDILIB PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/plugin>
        $<INSTALL_INTERFACE:include/Soundscape>
    )
    set_target_properties(MIDILIB PROPERTIES C_STANDARD 99 WINDOWS_EXPORT_ALL_SYMBOLS ON)
    set_target_properties(MIDILIB PROPERTIES FOLDER "Plugins")

    if(MSVC)
        target_compile_definitions(MIDILIB PRIVATE _CRT_SECURE_NO_WARNINGS)
    elseif(UNIX)
        target_link_libraries(MIDILIB PUBLIC m)
    endif()
endif()
