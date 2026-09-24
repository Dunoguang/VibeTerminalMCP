cmake_minimum_required(VERSION 3.16)
project(vibeterminalmcp-server CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(nlohmann_json REQUIRED)

add_executable(vibeterminalmcp-server
  src/main.cpp
  src/server.cpp
  src/terminal.cpp
  src/util.cpp
  src/stdio_transport.cpp
  src/http_transport.cpp
)

target_include_directories(vibeterminalmcp-server PRIVATE third_party)
target_link_libraries(vibeterminalmcp-server PRIVATE nlohmann_json::nlohmann_json pthread)
target_compile_options(vibeterminalmcp-server PRIVATE -Wall -Wextra)
