extends Node3D
## Optional LoxFX-owned Godauldron experiment. COMPOSES the neutral stage
## rather than inheriting its analytic seek callback: a stateful native hair
## solver must rewind and replay, never leap to an uncomputed physics state.

const GROOM_PATH := "res://experiments/loxfx/assets/Ratboy_mohawk.tfx"
const SHADER_DIR := "res://experiments/loxfx/shaders/"
const SWAY_METERS := 0.18

var _params: Dictionary = {}
var _lox_groom: Variant
var _visual_scale: float = 1.0
var _fit_origin: Vector3
var _subject_origin: Vector3
var _elapsed: float = 0.0
var _running: bool = false
var _error: String = ""

@onready var _stage: Node3D = $PortraitStage
@onready var _subject: Node3D = $PortraitStage/SubjectMotion
@onready var _anchor: Node3D = $PortraitStage/SubjectMotion/SubjectAnchor


func _ready() -> void:
    _subject_origin = _subject.position
    if not ClassDB.class_exists("LoxFXGroom3D"):
        _error = "LoxFXGroom3D extension not loaded"
        push_error(_error)
        return
    _lox_groom = ClassDB.instantiate("LoxFXGroom3D")
    if _lox_groom == null:
        _error = "Could not instantiate LoxFXGroom3D"
        push_error(_error)
        return
    _anchor.add_child(_lox_groom)
    godauldron_setup({"motion": "still", "followers": 2,
        "show_placeholder": true, "background": "as_authored",
        "gravity": 9.81, "wind_x": 0.0})


func _stage_only_parameters(values: Dictionary) -> Dictionary:
    var presentation: Dictionary = values.duplicate(true)
    # Turn/nod are deliberately not represented as physically credible
    # native scalp rotations: the SDK attachment stage has translation only.
    presentation["motion"] = "still"
    return presentation


func _shader_file() -> String:
    return SHADER_DIR + ("loxfx_integration.dxil"
        if OS.get_name() == "Windows" else "loxfx_integration.spv")


func _fit_to_placeholder() -> void:
    var points: PackedVector3Array = _lox_groom.get_initial_positions()
    if points.is_empty():
        return
    var low: Vector3 = points[0]
    var high: Vector3 = points[0]
    for p in points:
        low = low.min(p)
        high = high.max(p)
    var span: Vector3 = high - low
    # Simple fixture normalization, not a scalp/rig binding algorithm.
    _visual_scale = 1.3 / maxf(maxf(span.x, span.z), 0.01)
    _fit_origin = Vector3(0, 0.4, 0) - (low + high) * (0.5 * _visual_scale)
    _lox_groom.scale = Vector3.ONE * _visual_scale
    _lox_groom.position = _fit_origin


func godauldron_preview(values: Dictionary) -> void:
    _params = values.duplicate(true)
    _stage.godauldron_preview(_stage_only_parameters(values))


func godauldron_setup(values: Dictionary) -> void:
    _params = values.duplicate(true)
    _running = false
    _error = ""
    _elapsed = 0.0
    _stage.godauldron_setup(_stage_only_parameters(values))
    _subject.position = _subject_origin
    if _lox_groom == null:
        return
    var bytes: PackedByteArray = FileAccess.get_file_as_bytes(GROOM_PATH)
    if bytes.is_empty():
        _error = "Missing real TressFX asset: " + GROOM_PATH
        push_error(_error)
        return
    if not _lox_groom.load_tfx_bytes(
            bytes, int(values.get("followers", 2)), 0.02, 0.4, 17):
        _error = _lox_groom.get_last_error()
        push_error(_error)
        return
    _fit_to_placeholder()
    if not _lox_groom.start_native_diagnostic(_shader_file()):
        _error = _lox_groom.get_last_error()
        push_error(_error)
        return
    _running = true


func godauldron_step(dt: float) -> void:
    if not _running:
        return
    _elapsed += dt
    _stage.godauldron_seek(_elapsed) # Presentation only: the stage stays still.
    var displacement := 0.0
    if String(_params.get("motion", "still")) == "sway":
        displacement = SWAY_METERS * sin(_elapsed * 1.4)
    _subject.position = _subject_origin + Vector3(displacement, 0, 0)
    # Remove one copy of the parent translation. Native simulated roots
    # receive the displacement in original TFX units below.
    _lox_groom.position = _fit_origin - Vector3(displacement, 0, 0)
    var translation_in_asset_space := displacement / _visual_scale
    if not _lox_groom.step_native_diagnostic(
            dt, float(_params.get("gravity", 9.81)), 0.08,
            Vector3(translation_in_asset_space, 0, 0),
            Vector3(float(_params.get("wind_x", 0.0)), 0, 0)):
        _running = false
        _error = _lox_groom.get_last_error()
        push_error(_error)


# Intentionally no godauldron_seek callback.
# Godauldron ExperimentPlayback uses bounded reset/replay for stateful solvers.

func godauldron_inspection_bounds() -> AABB:
    return _stage.godauldron_inspection_bounds()


func godauldron_capture_metadata() -> Dictionary:
    var details: Dictionary = _stage.godauldron_capture_metadata()
    details["provider"] = "LoxFX"
    details["visualization_path"] = "diagnostic_cpu_bridge"
    details["native_simulation"] = true
    details["native_backend"] = (_lox_groom.get_native_diagnostic_backend()
        if _lox_groom != null else "none")
    details["simulated_steps"] = (_lox_groom.get_native_diagnostic_steps()
        if _lox_groom != null else 0)
    details["stage_time_seconds"] = _elapsed
    details["running"] = _running
    details["error"] = _error
    details["gpu_performance"] = "unmeasured"
    return details


func godauldron_metrics() -> Dictionary:
    return {
        "provider": "LoxFX",
        "running": _running,
        "simulated_steps": (_lox_groom.get_native_diagnostic_steps()
            if _lox_groom != null else 0),
        "visualization": "native_gpu_then_cpu_readback_then_godot_lines",
        "gpu_performance": "unmeasured",
        "error": _error
    }
