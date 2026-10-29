# Starter application: link application code to the HAL library.
add_executable(board_diagnostics src/main.c src/diagnostics.c)
target_include_directories(board_diagnostics PRIVATE include)
target_link_libraries(board_diagnostics PRIVATE hal Threads::Threads m)
set(DEPLOY_DIR "" CACHE PATH "Optional directory to copy the executable into")
if(DEPLOY_DIR)
    add_custom_command(TARGET board_diagnostics POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEPLOY_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:board_diagnostics>" "${DEPLOY_DIR}")
endif()
