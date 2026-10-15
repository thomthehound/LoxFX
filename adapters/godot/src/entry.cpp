// SPDX-FileCopyrightText: Copyright (c) 2026 thomthehound
// SPDX-License-Identifier: Apache-2.0

#include "loxfx_groom_3d.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static void loxfx_initialize(ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE)
        GDREGISTER_CLASS(loxfx::godot_adapter::LoxFXGroom3D);
}
static void loxfx_uninitialize(ModuleInitializationLevel) {}

extern "C" {
GDExtensionBool GDE_EXPORT loxfx_library_init(
        GDExtensionInterfaceGetProcAddress get_proc_address,
        GDExtensionClassLibraryPtr library,
        GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject initializer(
        get_proc_address, library, initialization);
    initializer.register_initializer(loxfx_initialize);
    initializer.register_terminator(loxfx_uninitialize);
    initializer.set_minimum_library_initialization_level(
        MODULE_INITIALIZATION_LEVEL_SCENE);
    return initializer.init();
}
}
