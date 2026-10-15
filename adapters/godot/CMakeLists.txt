# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0
#
# Opt-in Godot binding: remains separate from LoxFX::Core, the native GPU
# executors and Godauldron. Supply godot-cpp built for the actual host API.
if(NOT LOXFX_GODOT_CPP_SOURCE_DIR OR
   NOT EXISTS "${LOXFX_GODOT_CPP_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "LOXFX_BUILD_GODOT_ADAPTER requires LOXFX_GODOT_CPP_SOURCE_DIR "
    "pointing to an existing godot-cpp checkout")
endif()

# godot-cpp exposes godot::cpp as its CMake target alias.
add_subdirectory("${LOXFX_GODOT_CPP_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp")
add_library(loxfx_godot SHARED
  src/entry.cpp
  src/loxfx_groom_3d.cpp)
target_link_libraries(loxfx_godot PRIVATE LoxFX::Core godot::cpp)
target_compile_features(loxfx_godot PRIVATE cxx_std_23)
set_target_properties(loxfx_godot PROPERTIES
  OUTPUT_NAME loxfx_godot
  POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_compile_options(loxfx_godot PRIVATE -Wall -Wextra -Wpedantic)
endif()
