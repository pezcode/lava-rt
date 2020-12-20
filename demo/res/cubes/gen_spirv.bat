@ECHO on

glslangValidator -V cubes.frag
glslangValidator -V cubes.vert
glslangValidator -V --target-env spirv1.4 cubes.rgen
glslangValidator -V --target-env spirv1.4 cubes.rchit
glslangValidator -V --target-env spirv1.4 cubes.rmiss
glslangValidator -V --target-env spirv1.4 cubes.rcall
