# Test project for saige_vulkan_headers_usable: configure with -DFAKE_INCLUDE=<dir> and read HEADERS_USABLE from the output.
cmake_minimum_required(VERSION 3.21)
project(saige_header_selection_test LANGUAGES CXX)
set(SAIGE_VULKAN_MIN_HEADER_VERSION 303)
include("${SAIGE_VULKAN_HEADERS_MODULE}")
saige_vulkan_headers_usable("${FAKE_INCLUDE}" _usable)
message(STATUS "HEADERS_USABLE=${_usable}")
