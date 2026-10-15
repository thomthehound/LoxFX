extends "res://scripts/showcase_stage.gd"
## LoxFX-owned optional Godauldron experiment. Scene presentation is Godot;
## simulation is the genuine native LoxFX SDK-owned diagnostic executor.
## The first LoxFX root-motion primitive is TRANSLATION ONLY; this demo does
## not fake rotational/skinned attachments by spinning the full hair mesh.

const GROOM_PATH := "res://experiments/loxfx/assets/Ratboy_mohawk.tfx"
const SHADER_DIR := "res://experiments/loxfx/shaders/"
const DYNAMIC_AMPLITUDE_METERS := 0.18

var _lab_params: Dictionary = {}
var _lox_groom: Variant
var _visual_scale: float = 1.0
var _fitted_local_position: Vector3
var _subject_initial: Vector3
var _elapsed: float = 0.0
var _ready_to_step: bool = false
var _last_failure: String = ""


func _ready() -> void:
    super._ready()
    _subject_initial = $SubjectMotion.position
    if not ClassDB.class_exists("LoxFXGroom3D"):
        _last_failure = "LoxFXGroom3D GDExtension is not loaded"
        push_error(_last_failure)
        return
    _lox_groom = ClassDB.instantiate("LoxFXGroom3D")
    if _lox_groom == null:
        _last_failure = "Unable to instantiate LoxFXGroom3D"
        push_error(_last_failure)
        return
    $SubjectMotion/SubjectAnchor.add_child(_lox_groom)
    godauldron_setup({"motion": "still", "show_placeholder": true,
                      "background": "as_authored", "followers": 2})


func _shader_file() -> String:
    return SHADER_DIR + ("loxfx_integration.dxil"
        if OS.get_name() == "Windows" else "loxfx_integration.spv")


func _fit_to_head() -> void:
    var points: PackedVector3Array = _lox_groom.get_initial_positions()
    if points.is_empty():
        return
    var low := points[0]
    var high := points[0]
    for p in points:
        low = low.min(p)
        high = high.max(p)
    var extent := high - low
    var horizontal := maxf(extent.x, extent.z)
    # Fit the diagnostic groom roughly to a 1.4m-wide head. This is a visual
    # fixture only, not an asset semantic/scalp registration algorithm.
    _visual_scale = 1.3 / maxf(horizontal, 0.01)
    var center := (low + high) * 0.5
    _fitted_local_position = Vector3(0, 0.4, 0) - center * _visual_scale
    _lox_groom.scale = Vector3.ONE * _visual_scale
    _lox_groom.position = _fitted_local_position


func godauldron_preview(parameters: Dictionary) -> void:
    _lab_params = parameters.duplicate(true)
    # Only the neutral stage's still pose is used: turn/nod would rotate
    # line geometry as a rigid body and misrepresent simulated root motion.
    var stage_params: Dictionary = parameters.duplicate(true)
    stage_params["motion"] = "still"
    super.godauldron_preview(stage_params)


func godauldron_setup(parameters: Dictionary) -> void:
    _ready_to_step = false
    _elapsed = 0.0
    _last_failure = ""
    _lab_params = parameters.duplicate(true)
    var stage_params: Dictionary = parameters.duplicate(true)
    stage_params["motion"] = "still"
    super.godauldron_setup(stage_params)
    $SubjectMotion.position = _subject_initial
    if _lox_groom == null:
        return
    var bytes: PackedByteArray = FileAccess.get_file_as_bytes(GROOM_PATH)
    if bytes.is_empty():
        _last_failure = "Missing real groom at " + GROOM_PATH
        push_error(_last_failure)
        return
    if not _lox_groom.load_tfx_bytes(bytes, int(parameters.get("followers", 2)),
                                    0.02, 0.4, 17):
        _last_failure = _lox_groom.get_last_error()
        push_error(_last_failure)
        return
    _fit_to_head()
    if not _lox_groom.start_native_diagnostic(_shader_file()):
        _last_failure = _lox_groom.get_last_error()
        push_error(_last_failure)
        return
    _ready_to_step = true


func godauldron_step(dt: float) -> void:
    if not _ready_to_step:
        return
    _elapsed += dt
    super.godauldron_seek(_elapsed)
    var movement := 0.0
    if String(_lab_params.get("motion", "still")) == "sway":
        movement = DYNAMIC_AMPLITUDE_METERS * sin(_elapsed * 1.4)
    # The head translates in world space. Countertranslation of the
    # diagnostic mesh node removes double application of the same motion,
    # while native root translation actually moves the simulated roots.
    $SubjectMotion.position = _subject_initial + Vector3(movement, 0, 0)
    _lox_groom.position = _fitted_local_position - Vector3(movement, 0, 0)
    var native_dx: float = movement / _visual_scale
    if not _lox_groom.step_native_diagnostic(
            dt, float(_params.get("gravity", 9.81)), 0.08,
            Vector3(native_dx, 0, 0),
            Vector3(float(_params.get("wind_x", 0.0)), 0, 0)):
        _ready_to_step = false
        _last_failure = _lox_groom.get_last_error()
        push_error(_last_failure)


# A stateful GPU solver has no arbitrary analytical seek. Explicitly
# rewind and replay bounded fixed steps; never use the inherited stage
# implementation to teleport a simulated groom to a new timeline position.
func godauldron_seek(seconds: float) -> void:
    var target: int = int(round(maxf(seconds, 0.0) * 60.0))
    if target > 1200:
        _last_failure = "Diagnostic GPU replay capped at 1200 steps"
        push_warning(_last_failure)
        return
    var existing: int = int(round(_elapsed * 60.0))
    if target < existing:
        godauldron_setup(_lab_params)
        existing = 0
    while existing < target and _ready_to_step:
        godauldron_step(1.0 / 60.0)
        existing += 1


func godauldron_capture_metadata() -> Dictionary:
    var info: Dictionary = super.godauldron_capture_metadata()
    info["provider"] = "LoxFX"
    info["visualization_path"] = "diagnostic_cpu_bridge"
    info["sdk_owned_native_simulation"] = true
    info["simulation_running"] = _ready_to_step
    info["simulation_steps"] = (_lox_groom.get_native_diagnostic_steps()
        if _lox_groom != null else 0)
    info["simulation_backend"] = (_lox_groom.get_native_diagnostic_backend()
        if _lox_groom != null else "none")
    info["last_error"] = _last_failure
    info["frame_timing"] = "unmeasured; synchronous native GPU and CPU readback"
    return info


func godauldron_metrics() -> Dictionary:
    return {
        "simulation_running": _ready_to_step,
        "simulated_steps": (_lox_groom.get_native_diagnostic_steps()
            if _lox_groom != null else 0),
        "native_gpu_performance": "unmeasured",
        "diagnostic_display": "cpu_readback_and_godot_line_upload",
        "error": _last_failure
    }
