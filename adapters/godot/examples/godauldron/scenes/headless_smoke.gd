extends SceneTree
## Cross-repository smoke: instantiate the real provider-owned hair scene
## under Godauldron's genuine portrait stage and advance native compute.

func _initialize() -> void:
    call_deferred("_run")

func _fail(message: String) -> void:
    push_error(message)
    quit(1)

func _run() -> void:
    if not ClassDB.class_exists("LoxFXGroom3D"):
        _fail("LoxFX native adapter was not registered in Godauldron")
        return
    var packed: PackedScene = load(
        "res://experiments/loxfx/scenes/hair_lab.tscn")
    if packed == null:
        _fail("LoxFX portrait-stage scene could not load")
        return
    var scene: Variant = packed.instantiate()
    if scene == null:
        _fail("LoxFX portrait-stage scene could not instantiate")
        return
    root.add_child(scene)
    var details: Dictionary = scene.godauldron_capture_metadata()
    if not bool(details.get("running", false)):
        _fail("LoxFX native provider failed initial setup: " +
            String(details.get("error", "")))
        return

    scene.godauldron_setup({
        "motion": "sway", "followers": 1,
        "show_placeholder": true, "background": "as_authored",
        "gravity": 9.81, "wind_x": 0.0
    })
    scene.godauldron_step(1.0 / 60.0)
    details = scene.godauldron_capture_metadata()
    if (not bool(details.get("running", false))
            or int(details.get("simulated_steps", 0)) != 1):
        _fail("LoxFX's Godauldron scene did not execute native GPU hair: " +
            String(details.get("error", "")))
        return
    if String(details.get("visualization_path", "")) != "diagnostic_cpu_bridge":
        _fail("Native diagnostic visualization is incorrectly identified")
        return
    print("PASS: Godauldron portrait stage + real TFX GPU simulation + ",
        "accurate diagnostic metadata")
    quit(0)
