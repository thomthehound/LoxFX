# SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28)
project(LoxFXPackageSmoke LANGUAGES CXX)

find_package(LoxFX CONFIG REQUIRED)

add_executable(loxfx_package_smoke main.cpp)
target_link_libraries(loxfx_package_smoke PRIVATE LoxFX::Core)
