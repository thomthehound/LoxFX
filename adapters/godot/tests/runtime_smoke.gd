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
    if not instance.is_loaded() or instance.get_authored_guide_count() != 2228         or instance.get_vertices_per_strand() != 32         or instance.get_followers_per_guide() != 2:
        _fail("Imported real TFX groom has incorrect topology")
        return

    var points: PackedVector3Array = instance.get_initial_positions()
    if points.size() != instance.get_packed_vertex_count()         or points.size() != 2240 * 32 * 3:
        _fail("Prepared packed groom lost its guide/follower/padding layout")
        return
    if instance.mesh == null or instance.mesh.get_surface_count() != 1:
        _fail("Failed to create diagnostic line mesh")
        return

    points[1] += Vector3(0.0, 0.02, 0.0)
    if not instance.publish_positions(points):
        _fail("Failed Godot dynamic mesh update: " + instance.get_last_error())
        return
    if not instance.get_last_error().is_empty()         or instance.mesh.get_surface_count() != 1:
        _fail("Dynamic position update damaged the existing mesh")
        return

    print("PASS: native Godot extension loaded, real TFX groom prepared, ",
        "and diagnostic strand buffer updated")
    quit(0)
