# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28)
project(LoxFX VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(LOXFX_ENABLE_VULKAN "Build the Vulkan integration stage and diagnostic runner" OFF)

add_library(loxfx_core STATIC
  src/tfx_import.cpp
  src/simulation_work.cpp
  src/prepared_groom.cpp)
add_library(LoxFX::Core ALIAS loxfx_core)
target_compile_features(loxfx_core PUBLIC cxx_std_23)
target_include_directories(loxfx_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(loxfx_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  EXPORT_NAME Core)
if(MSVC)
  target_compile_options(loxfx_core PRIVATE /W4)
else()
  target_compile_options(loxfx_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(LOXFX_BUILD_TOOLS "Build the standalone groom inspection tool" ON)
if(LOXFX_BUILD_TOOLS)
  add_executable(loxfx_tfx_inspect tools/tfx_inspect.cpp)
  target_link_libraries(loxfx_tfx_inspect PRIVATE LoxFX::Core)
endif()

if(BUILD_TESTING)
  add_executable(loxfx_tfx_import_tests tests/tfx_import_tests.cpp)
  target_link_libraries(loxfx_tfx_import_tests PRIVATE LoxFX::Core)
  add_test(NAME loxfx_tfx_import COMMAND loxfx_tfx_import_tests)

  add_executable(loxfx_simulation_work_tests tests/simulation_work_tests.cpp)
  target_link_libraries(loxfx_simulation_work_tests PRIVATE LoxFX::Core)
  add_test(NAME loxfx_simulation_work COMMAND loxfx_simulation_work_tests)

  add_executable(loxfx_prepared_groom_tests tests/prepared_groom_tests.cpp)
  target_link_libraries(loxfx_prepared_groom_tests PRIVATE LoxFX::Core)
  add_test(NAME loxfx_prepared_groom COMMAND loxfx_prepared_groom_tests)
endif()

# Public, relocatable CMake package for independently built consumers.
install(TARGETS loxfx_core EXPORT LoxFXTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT LoxFXTargets
  FILE LoxFXTargets.cmake
  NAMESPACE LoxFX::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LoxFX)
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LoxFXConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/LoxFXConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LoxFX")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/LoxFXConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/LoxFXConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/LoxFXConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LoxFX")

# First native Vulkan *simulation stage* (opt-in): compile HLSL with DXC.
# CPU import/SDK builds do not require Vulkan or a shader compiler.
# Option is declared above the generated package config so consumers
# receive the correct find_dependency(Vulkan) contract.
if(LOXFX_ENABLE_VULKAN)
  find_package(Vulkan REQUIRED)
  find_program(LOXFX_DXC_EXECUTABLE NAMES dxc dxc.exe REQUIRED)
  set(LOXFX_INTEGRATION_SPV "${CMAKE_CURRENT_BINARY_DIR}/shaders/loxfx_integration.spv")
  add_custom_command(
    OUTPUT "${LOXFX_INTEGRATION_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${LOXFX_DXC_EXECUTABLE}"
      -spirv -T cs_6_6 -E IntegrationAndGlobalShapeConstraints
      -fspv-target-env=vulkan1.2
      -Fo "${LOXFX_INTEGRATION_SPV}"
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/IntegrationAndGlobalShapeConstraints.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/IntegrationAndGlobalShapeConstraints.hlsl"
    VERBATIM)
  add_custom_target(loxfx_integration_shader DEPENDS "${LOXFX_INTEGRATION_SPV}")
  add_library(loxfx_vulkan STATIC src/vulkan/integration.cpp)
  add_library(LoxFX::Vulkan ALIAS loxfx_vulkan)
  add_dependencies(loxfx_vulkan loxfx_integration_shader)
  target_link_libraries(loxfx_vulkan PUBLIC LoxFX::Core PRIVATE Vulkan::Vulkan)
  target_compile_features(loxfx_vulkan PUBLIC cxx_std_23)
  target_include_directories(loxfx_vulkan PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  set_target_properties(loxfx_vulkan PROPERTIES
    EXPORT_NAME Vulkan POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    target_compile_options(loxfx_vulkan PRIVATE /W4)
  else()
    target_compile_options(loxfx_vulkan PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  # Vulkan is private because backend-specific types do not escape the
  # public header. The linked Vulkan loader is still required at runtime.
  install(TARGETS loxfx_vulkan EXPORT LoxFXTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES "${LOXFX_INTEGRATION_SPV}" DESTINATION "${CMAKE_INSTALL_DATADIR}/loxfx/shaders")
  if(BUILD_TESTING)
    add_executable(loxfx_vulkan_integration_tests tests/vulkan_integration_tests.cpp)
    target_link_libraries(loxfx_vulkan_integration_tests PRIVATE LoxFX::Vulkan)
    add_dependencies(loxfx_vulkan_integration_tests loxfx_integration_shader)
    add_test(NAME loxfx_vulkan_integration
      COMMAND loxfx_vulkan_integration_tests "${LOXFX_INTEGRATION_SPV}")
  endif()
endif()
