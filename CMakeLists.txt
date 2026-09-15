cmake_minimum_required(VERSION 3.16)
project(serial-data-gateway CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_executable(serial-data-gateway
  src/main.cpp
  src/frame.cpp
  src/source.cpp
  src/storage.cpp
  src/tcp_server.cpp
)

target_link_libraries(serial-data-gateway PRIVATE Threads::Threads)
