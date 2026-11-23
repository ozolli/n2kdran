cmake_minimum_required(VERSION 3.18)
project(n2kjni C)

# Cœur partagé avec le build Linux (tests, n2kdump).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../libn2k libn2k)

add_library(n2kjni SHARED n2k_jni.c)
set_target_properties(n2kjni PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(n2kjni PRIVATE -Wall -Wextra)
target_link_libraries(n2kjni n2k log)
