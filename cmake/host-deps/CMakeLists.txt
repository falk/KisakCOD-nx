# Host builds of MojoShader and UAM for the deko9 shader tests, from the same
# recipe as the NRO (cmake/switch-deps.cmake).
cmake_minimum_required(VERSION 3.21)
project(kisak-host-deps C CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/../switch-deps.cmake")
