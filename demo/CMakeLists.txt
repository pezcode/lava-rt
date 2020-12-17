cmake_minimum_required(VERSION 3.15)

project(lava-rt-demo VERSION 0.5.5 LANGUAGES C CXX)

add_library(lava-rt.demo STATIC
        demo.hpp
        demo.cpp
        )
target_include_directories(lava-rt.demo PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        )
target_link_libraries(lava-rt.demo PUBLIC
        lava::app
        lava-extras::raytracing
        )
add_library(lava-rt::demo ALIAS lava-rt.demo)

set(CUBES_SHADERS
        res/cubes/cubes.rgen
        res/cubes/cubes.rchit
        res/cubes/cubes.rmiss
        res/cubes/cubes.vert
        res/cubes/cubes.frag
        res/cubes/cubes.inc
        )

add_executable(lava-rt-cubes
        cubes.cpp
        ${CUBES_SHADERS}
        )
target_link_libraries(lava-rt-cubes lava-rt::demo)
set_property(TARGET lava-rt-cubes PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")

source_group("Shader Files" FILES ${CUBES_SHADERS})

file(CREATE_LINK "${PROJECT_SOURCE_DIR}/res" "${PROJECT_BINARY_DIR}/res" COPY_ON_ERROR SYMBOLIC)
