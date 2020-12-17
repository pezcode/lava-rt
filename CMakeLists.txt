cmake_minimum_required(VERSION 3.15)
project(lava-rt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE) 
    set(CMAKE_BUILD_TYPE Release)
endif()

#set(BUILD_SHARED_LIBS OFF)

add_subdirectory(ext)
add_subdirectory(liblava-extras)
add_subdirectory(demo)

set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT lava-rt-cubes)
