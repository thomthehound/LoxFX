extends SceneTree

func _initialize() -> void:
    call_deferred("_run")

func _run() -> void:
    assert(ClassDB.class_exists("LoxFXGroom3D"),
        "GDExtension class did not register")
    var instance: Node = ClassDB.instantiate("LoxFXGroom3D")
    assert(instance != null, "Could not instantiate native groom node")
    root.add_child(instance)

    var source: PackedByteArray = FileAccess.get_file_as_bytes(
        "res://grooms/Ratboy_mohawk.tfx")
    assert(source.size() > 0, "Missing real TressFX groom")
    assert(instance.load_tfx_bytes(source, 2, 0.03, 0.6, 42),
        "TFX import/preparation failed: " + instance.get_last_error())
    assert(instance.is_loaded())
    assert(instance.get_authored_guide_count() == 2228)
    assert(instance.get_vertices_per_strand() == 32)
    assert(instance.get_followers_per_guide() == 2)

    var points: PackedVector3Array = instance.get_initial_positions()
    assert(points.size() == instance.get_packed_vertex_count())
    assert(points.size() == 2240 * 32 * 3)
    assert(instance.mesh != null)
    assert(instance.mesh.get_surface_count() == 1)

    # Verify the actual Godot dynamic mesh update entry point is callable.
    points[1] += Vector3(0.0, 0.02, 0.0)
    assert(instance.publish_positions(points), "Failed dynamic mesh update")
    assert(instance.get_last_error().is_empty())
    assert(instance.mesh.get_surface_count() == 1)
    print("PASS: native Godot extension loaded, real TFX groom prepared, ",
        "and diagnostic strand buffer updated")
    quit(0)
