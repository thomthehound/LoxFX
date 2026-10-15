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
# The top-level CMake project selects CMAKE_MSVC_RUNTIME_LIBRARY before
# it creates any targets when this adapter is enabled. Do not fix up only
# LoxFX::Core here: that would leave other linked tools/tests mismatched.
if(MSVC)
  get_target_property(_godot_runtime godot-cpp MSVC_RUNTIME_LIBRARY)
  get_target_property(_core_runtime loxfx_core MSVC_RUNTIME_LIBRARY)
  if(NOT _godot_runtime STREQUAL _core_runtime)
    message(FATAL_ERROR
      "The Godot adapter and LoxFX Core require one MSVC runtime; "
      "set CMAKE_MSVC_RUNTIME_LIBRARY consistently for the whole build")
  endif()
endif()
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
