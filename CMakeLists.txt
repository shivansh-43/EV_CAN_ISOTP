
cmake_minimum_required(VERSION 3.10)

project(EV_CAN_ISOTP C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(include)

add_library(isotp
    src/isotp.c
)

add_executable(test_isotp
    examples/main.c
)

target_link_libraries(test_isotp isotp)
add_executable(test_two_contexts examples/test_two_contexts.c)
target_link_libraries(test_two_contexts isotp)
add_executable(test_block_size examples/test_block_size.c)
target_link_libraries(test_block_size isotp)
add_executable(test_block_size_zero examples/test_block_size_zero.c)
target_link_libraries(test_block_size_zero isotp)