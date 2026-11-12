set(installTargets SDLIB)
if(SUPPORT_MIDI)
    list(APPEND installTargets MIDILIB)
endif()

install(TARGETS ${installTargets}
    EXPORT Soundscape
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    FRAMEWORK DESTINATION lib
)

file(GLOB interfaceHeaders
    ${CMAKE_CURRENT_SOURCE_DIR}/core/*.h
)
list(APPEND interfaceHeaders "support/support_def.h")
if(SUPPORT_MIDI)
    list(APPEND interfaceHeaders ${CMAKE_CURRENT_SOURCE_DIR}/plugin/MIDI.h)
endif()

install(FILES ${interfaceHeaders} DESTINATION include/Soundscape)
install(EXPORT Soundscape DESTINATION lib/Soundscape FILE SoundscapeConfig.cmake)
