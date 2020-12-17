#!/bin/bash

glslangValidator --target-env vulkan1.1 cubes.frag
glslangValidator --target-env vulkan1.1 cubes.vert
glslangValidator --target-env vulkan1.1 cubes.rgen
glslangValidator --target-env vulkan1.1 cubes.rchit
glslangValidator --target-env vulkan1.1 cubes.rmiss
