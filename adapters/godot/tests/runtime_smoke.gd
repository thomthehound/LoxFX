extends SceneTree

func _initialize() -> void:
    call_deferred("_run")

func _fail(message: String) -> void:
    push_error(message)
    quit(1)

func _run() -> void:
    if not ClassDB.class_exists("LoxFXGroom3D"):
        _fail("GDExtension class did not register")
        return

    # Remain dynamically typed: MeshInstance3D's static type does not
    # declare the extra native methods provided by this GDExtension.
    var instance: Variant = ClassDB.instantiate("LoxFXGroom3D")
    if instance == null:
        _fail("Could not instantiate native LoxFXGroom3D")
        return
    root.add_child(instance)

    var source: PackedByteArray = FileAccess.get_file_as_bytes(
        "res://grooms/Ratboy_mohawk.tfx")
    if source.is_empty():
        _fail("Missing real AMD TressFX groom")
        return
    if not instance.load_tfx_bytes(source, 2, 0.03, 0.6, 42):
        _fail("TFX import/preparation failed: " + instance.get_last_error())
        return
    if (not instance.is_loaded()
            or instance.get_authored_guide_count() != 2228
            or instance.get_vertices_per_strand() != 32
            or instance.get_followers_per_guide() != 2):
        _fail("Imported real TFX groom has incorrect topology")
        return

    var points: PackedVector3Array = instance.get_initial_positions()
    if (points.size() != instance.get_packed_vertex_count()
            or points.size() != 2240 * 32 * 3):
        _fail("Prepared packed groom lost its guide/follower/padding layout")
        return
    if instance.mesh == null or instance.mesh.get_surface_count() != 1:
        _fail("Failed to create diagnostic line mesh")
        return

    points[1] += Vector3(0.0, 0.02, 0.0)
    if not instance.publish_positions(points):
        _fail("Failed Godot dynamic mesh update: " + instance.get_last_error())
        return
    if (not instance.get_last_error().is_empty()
            or instance.mesh.get_surface_count() != 1):
        _fail("Dynamic position update damaged the existing mesh")
        return

    var expected_shader: String = "res://shaders/loxfx_integration.spv"
    var native_required: bool = OS.get_cmdline_user_args().has("--native-required")
    if native_required and not FileAccess.file_exists(expected_shader):
        _fail("Native diagnostic mode requires compiled Vulkan shader assets")
        return
    if native_required:
        var rest_root: Vector3 = points[0]
        if not instance.start_native_diagnostic(expected_shader):
            _fail("LoxFX native Vulkan startup failed: " + instance.get_last_error())
            return
        if not instance.is_native_diagnostic_active()                 or not instance.get_native_diagnostic_backend().contains("Vulkan"):
            _fail("Native Vulkan diagnostic provider did not initialize")
            return
        if not instance.step_native_diagnostic(
                1.0 / 60.0, 9.81, 0.08, Vector3(0.25, 0.0, 0.0)):
            _fail("Native LoxFX six-stage step failed: " + instance.get_last_error())
            return
        var root_result: Vector3 = instance.get_native_diagnostic_root_position()
        if instance.get_native_diagnostic_steps() != 1                 or absf(root_result.x - rest_root.x - 0.25) > 0.01:
            _fail("Native compute did not publish translated guide root")
            return
        if not instance.reset_native_diagnostic()                 or instance.get_native_diagnostic_steps() != 0                 or instance.get_native_diagnostic_root_position().distance_to(rest_root) > 0.0001:
            _fail("Native simulation state did not reset to source rest geometry")
            return
        instance.stop_native_diagnostic()
        if instance.is_native_diagnostic_active():
            _fail("Native diagnostic simulation did not release its device state")
            return

    print("PASS: real TFX groom, Godot mesh publication", 
        ", native Vulkan simulation/reset" if native_required else "")
    quit(0)
