cmake_minimum_required(VERSION 3.21)

project(
  SZLInspector
  VERSION 0.4.0
  DESCRIPTION "Diagnostic RED4ext plugin: logs how the game draws one frame (for the scope outline fix)"
  LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(RED4EXT_SDK_DIR "D:/RED4ext.SDK" CACHE PATH "Path to a checkout of RED4ext.SDK")
if(NOT EXISTS "${RED4EXT_SDK_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "RED4ext.SDK not found at ${RED4EXT_SDK_DIR}. Pass -DRED4EXT_SDK_DIR=<path>.")
endif()
set(RED4EXT_HEADER_ONLY ON CACHE BOOL "Use the header only version of the SDK." FORCE)
add_subdirectory("${RED4EXT_SDK_DIR}" "${CMAKE_BINARY_DIR}/red4ext.sdk" EXCLUDE_FROM_ALL)

# Vtbl.c is plain C: it reads the D3D12 method slots from the Windows SDK's C interface.
add_library(SZLInspector SHARED src/Main.cpp src/Vtbl.c)
target_link_libraries(SZLInspector PRIVATE RED4ext::SDK dxguid)
target_compile_definitions(SZLInspector PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
  target_compile_options(SZLInspector PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)
endif()

set_target_properties(
  SZLInspector
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../red4ext/plugins/SZLInspector"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../red4ext/plugins/SZLInspector"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb"
)
