function(copy_runtime_dependencies target_name)
    if(NOT WIN32)
        return()
    endif()

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:${target_name}>"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copying linked runtime DLLs for ${target_name}"
    )

    foreach(ffmpeg_dll IN LISTS FFMPEG_RUNTIME_DLLS)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ffmpeg_dll}"
                "$<TARGET_FILE_DIR:${target_name}>"
            COMMENT "Copying FFmpeg runtime DLL: ${ffmpeg_dll}"
        )
    endforeach()
endfunction()
