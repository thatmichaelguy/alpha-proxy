cmake_minimum_required(VERSION 3.12)
project(alpha-proxy)

# Add custom options with default values set to ON
option(WITH_IP_EXTENSION "Enable IP extension feature" ON)
option(ALLOW_ONLY_HAPROXY "Allow only connections from HAProxy" ON)

set(CMAKE_CXX_STANDARD 20)

# Set the default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

# Enable/disable optimizations and define DEBUG based on build type
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(DEBUG "Debug mode" ON)
    add_compile_options(-O0 -g)  # Disable optimizations for Debug
else()
    add_compile_options(-Ofast -march=native)
endif()

find_package(Boost REQUIRED COMPONENTS system)

add_executable(alpha-proxy main.cpp server.cpp)

# Conditional compilation based on the options
if(WITH_IP_EXTENSION)
    target_compile_definitions(alpha-proxy PRIVATE WITH_IP_EXTENSION)
endif()

if(ALLOW_ONLY_HAPROXY)
    target_compile_definitions(alpha-proxy PRIVATE ALLOW_ONLY_HAPROXY)
endif()

if(DEBUG)
    target_compile_definitions(alpha-proxy PRIVATE DEBUG)
endif()

target_link_libraries(alpha-proxy Boost::system pthread)
