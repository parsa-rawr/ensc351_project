add_executable(circuit_controller
    src/main.c
    src/controller.c
)
target_include_directories(circuit_controller PRIVATE include)
target_link_libraries(circuit_controller PRIVATE m)
