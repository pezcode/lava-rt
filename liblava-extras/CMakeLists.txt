cmake_minimum_required(VERSION 3.15)

project(liblava-extras VERSION 0.5.6 LANGUAGES C CXX)

set(LIBLAVA_EXTRAS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/liblava-extras)

message(">> lava-extras::core")

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/empty.cpp "")

add_library(lava-extras.core STATIC
        ${CMAKE_CURRENT_BINARY_DIR}/empty.cpp
        ${LIBLAVA_EXTRAS_DIR}/lava-extras.hpp
        ${LIBLAVA_EXTRAS_DIR}/raytracing.hpp
        )

target_include_directories(lava-extras.core INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        )

set_property(TARGET lava-extras.core PROPERTY EXPORT_NAME core)
add_library(lava-extras::core ALIAS lava-extras.core)

message(">> lava-extras::raytracing")

add_library(lava-extras.raytracing STATIC
        ${LIBLAVA_EXTRAS_DIR}/raytracing/acceleration_structure.hpp
        ${LIBLAVA_EXTRAS_DIR}/raytracing/acceleration_structure.cpp
        ${LIBLAVA_EXTRAS_DIR}/raytracing/pipeline.hpp
        ${LIBLAVA_EXTRAS_DIR}/raytracing/pipeline.cpp
        ${LIBLAVA_EXTRAS_DIR}/raytracing/shader_binding_table.hpp
        )

target_link_libraries(lava-extras.raytracing PUBLIC
        lava::resource
        lava::block
        lava-extras::core
        )

set_property(TARGET lava-extras.raytracing PROPERTY EXPORT_NAME raytracing)
add_library(lava-extras::raytracing ALIAS lava-extras.raytracing)
