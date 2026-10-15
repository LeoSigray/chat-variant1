cmake_minimum_required(VERSION 3.10)
project(chat C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(server server.c common.c common.h)
add_executable(client client.c common.c common.h)

if(WIN32)
    target_link_libraries(server ws2_32)
    target_link_libraries(client ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(server Threads::Threads)
    target_link_libraries(client Threads::Threads)
endif()

if(MSVC)
    # /utf-8 обязателен: русские строки должны уходить в сеть в UTF-8
    target_compile_options(server PRIVATE /utf-8 /W4)
    target_compile_options(client PRIVATE /utf-8 /W4)
else()
    target_compile_options(server PRIVATE -Wall -Wextra -pedantic)
    target_compile_options(client PRIVATE -Wall -Wextra -pedantic)
endif()
