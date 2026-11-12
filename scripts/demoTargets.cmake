# Building Examples

add_executable(Writer examples/Writer.c)
add_executable(Listener examples/Listener.c)

list(APPEND demo_targets Writer Listener)

foreach(target IN ITEMS ${demo_targets})
    # message(STATUS "Including ${miniaudio_h} and ${EXTERNAL_PROJ_DIR}/kissfft/src")
    target_include_directories(${target} PUBLIC core support)
    target_include_directories(${target} PUBLIC ${CMAKE_INSTALL_PREFIX}/include ${EXTERNAL_PROJ_DIR}/kissfft/src)
    target_link_libraries(${target} PRIVATE SDLIB)
    target_link_libraries(${target} PUBLIC ${miniaudio_lib})
    set_target_properties(${target} PROPERTIES C_STANDARD 99)
    set_target_properties(${target} PROPERTIES FOLDER "Examples")
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(SUPPORT_MIDI) # linking examples to the MIDI plugin
        target_link_libraries(${target} PRIVATE MIDILIB)
    endif()
endforeach()
