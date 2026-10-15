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

option(LOXFX_BUILD_VULKAN_SMOKE "Build experimental Vulkan compute smoke test" OFF)
# DXIL compilation is independent of Vulkan: prove shared HLSL portability
# before the native D3D12 runtime is implemented.
option(LOXFX_ENABLE_D3D12_SHADERS "Compile the shared integration stage to DXIL" OFF)
option(LOXFX_BUILD_D3D12_SMOKE "Build internal native D3D12 stage-one test" OFF)

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
# Prototype backend headers are not installed as public SDK APIs.
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  PATTERN "vulkan" EXCLUDE
  PATTERN "d3d12" EXCLUDE)
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

# Experimental compute proof: not an exported backend target.
# CPU import/SDK builds do not require Vulkan or DXC.
if(LOXFX_BUILD_VULKAN_SMOKE)
  find_package(Vulkan REQUIRED)
  find_program(LOXFX_DXC_EXECUTABLE NAMES dxc dxc.exe REQUIRED)
  set(LOXFX_INTEGRATION_SPV "${CMAKE_CURRENT_BINARY_DIR}/shaders/loxfx_integration.spv")
  add_custom_command(
    OUTPUT "${LOXFX_INTEGRATION_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${LOXFX_DXC_EXECUTABLE}"
      -spirv -T cs_6_6 -E IntegrationAndGlobalShapeConstraints -DLOXFX_SPIRV=1
      -fspv-target-env=vulkan1.2
      -Fo "${LOXFX_INTEGRATION_SPV}"
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/IntegrationAndGlobalShapeConstraints.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/IntegrationAndGlobalShapeConstraints.hlsl"
    VERBATIM)
  add_custom_target(loxfx_integration_shader DEPENDS "${LOXFX_INTEGRATION_SPV}")
  set(LOXFX_STRAND_SPV "${CMAKE_CURRENT_BINARY_DIR}/shaders/loxfx_strand_level.spv")
  add_custom_command(
    OUTPUT "${LOXFX_STRAND_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${LOXFX_DXC_EXECUTABLE}" -spirv -T cs_6_6
      -E CalculateStrandLevelData -DLOXFX_SPIRV=1 -fspv-target-env=vulkan1.2
      -Fo "${LOXFX_STRAND_SPV}"
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/CalculateStrandLevelData.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/CalculateStrandLevelData.hlsl"
    VERBATIM)
  add_custom_target(loxfx_strand_shader DEPENDS "${LOXFX_STRAND_SPV}")
  add_library(loxfx_vulkan_smoke STATIC src/vulkan/smoke_context.cpp)
  add_dependencies(loxfx_vulkan_smoke loxfx_integration_shader loxfx_strand_shader)
  # Internal smoke header exposes VkCommandBuffer; test consumers need Vulkan headers.
  target_link_libraries(loxfx_vulkan_smoke PUBLIC LoxFX::Core Vulkan::Vulkan)
  target_compile_features(loxfx_vulkan_smoke PUBLIC cxx_std_23)
  target_include_directories(loxfx_vulkan_smoke PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  set_target_properties(loxfx_vulkan_smoke PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    target_compile_options(loxfx_vulkan_smoke PRIVATE /W4)
  else()
    target_compile_options(loxfx_vulkan_smoke PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  # Neither prototype library nor its shader is installed or exported.
  if(BUILD_TESTING)
    add_executable(loxfx_vulkan_integration_tests tests/vulkan_integration_tests.cpp)
    target_link_libraries(loxfx_vulkan_integration_tests PRIVATE loxfx_vulkan_smoke)
    add_dependencies(loxfx_vulkan_integration_tests loxfx_integration_shader)
    add_test(NAME loxfx_vulkan_integration
      COMMAND loxfx_vulkan_integration_tests "${LOXFX_INTEGRATION_SPV}" "${LOXFX_STRAND_SPV}")
  endif()
endif()

# D3D12 compiler parity: produce native SM6.6 DXIL from the same shader
# sources as Vulkan. Native D3D12 execution is a separate milestone.
if(LOXFX_ENABLE_D3D12_SHADERS OR LOXFX_BUILD_D3D12_SMOKE)
  if(NOT WIN32)
    message(FATAL_ERROR "D3D12 DXIL integration-shader validation currently requires Windows")
  endif()
  find_program(LOXFX_DXC_EXECUTABLE NAMES dxc dxc.exe REQUIRED)
  set(LOXFX_INTEGRATION_DXIL "${CMAKE_CURRENT_BINARY_DIR}/shaders/loxfx_integration.dxil")
  add_custom_command(
    OUTPUT "${LOXFX_INTEGRATION_DXIL}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${LOXFX_DXC_EXECUTABLE}"
      -T cs_6_6 -E IntegrationAndGlobalShapeConstraints
      -Fo "${LOXFX_INTEGRATION_DXIL}"
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/IntegrationAndGlobalShapeConstraints.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/IntegrationAndGlobalShapeConstraints.hlsl"
    VERBATIM)
  add_custom_target(loxfx_integration_dxil ALL DEPENDS "${LOXFX_INTEGRATION_DXIL}")
  set(LOXFX_STRAND_DXIL "${CMAKE_CURRENT_BINARY_DIR}/shaders/loxfx_strand_level.dxil")
  add_custom_command(
    OUTPUT "${LOXFX_STRAND_DXIL}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${LOXFX_DXC_EXECUTABLE}" -T cs_6_6 -E CalculateStrandLevelData
      -Fo "${LOXFX_STRAND_DXIL}"
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/CalculateStrandLevelData.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/CalculateStrandLevelData.hlsl"
    VERBATIM)
  add_custom_target(loxfx_strand_dxil ALL DEPENDS "${LOXFX_STRAND_DXIL}")
  # DXIL alone can be installed for explicit compiler-parity consumers,
  # but never ship the experimental native runtime in an SDK package.
  if(LOXFX_ENABLE_D3D12_SHADERS AND NOT LOXFX_BUILD_D3D12_SMOKE)
    install(FILES "${LOXFX_INTEGRATION_DXIL}"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/loxfx/shaders")
  endif()
endif()

# Native D3D12 stage-one proof: separate C++23 backend, same HLSL algorithm.
# Internal target only. No descriptor heaps: root SRV/UAV descriptors and
# root constants map directly to the first-stage shader's t2/u0/u1/b0.
if(LOXFX_BUILD_D3D12_SMOKE)
  add_library(loxfx_d3d12_smoke STATIC src/d3d12/smoke_context.cpp)
  target_compile_features(loxfx_d3d12_smoke PUBLIC cxx_std_23)
  target_include_directories(loxfx_d3d12_smoke PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  target_link_libraries(loxfx_d3d12_smoke PUBLIC LoxFX::Core d3d12 dxgi)
  add_dependencies(loxfx_d3d12_smoke loxfx_integration_dxil loxfx_strand_dxil)
  if(MSVC)
    target_compile_options(loxfx_d3d12_smoke PRIVATE /W4)
  else()
    target_compile_options(loxfx_d3d12_smoke PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(BUILD_TESTING)
    add_executable(loxfx_d3d12_integration_tests tests/d3d12_integration_tests.cpp)
    target_link_libraries(loxfx_d3d12_integration_tests PRIVATE loxfx_d3d12_smoke)
    add_test(NAME loxfx_d3d12_integration
      COMMAND loxfx_d3d12_integration_tests "${LOXFX_INTEGRATION_DXIL}" "${LOXFX_STRAND_DXIL}")
  endif()
endif()
