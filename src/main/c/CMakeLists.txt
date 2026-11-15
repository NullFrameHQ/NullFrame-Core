cmake_minimum_required(VERSION 3.16)
project(nullframe_native_entry C)

# Keep one source list and compiler configuration for both native entry points.
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." "${CMAKE_CURRENT_BINARY_DIR}/core")
