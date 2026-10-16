extends SceneTree
## Godot 4.7.2 Forward+ integration proof (NOT production hair rendering).
## Executes LoxFX's ACTUAL integration and follower/tangent SPIR-V on the
## renderer-owned RenderingDevice, with Godot RIDs and its compute graph.
## No local RenderingDevice, raw Vulkan handles, external command submission,
## GPU->CPU upload per frame or LoxFX-specific Godot renderer fork.
## A ONE-TIME async readback is used only for numerical test acceptance.

const INTEGRATION := "res://shaders/loxfx_integration.spv"
const FOLLOWERS := "res://shaders/loxfx_follower_publish.spv"
const GUIDE_COUNT := 16
const VERTICES := 4
const BUFFER_FLOATS := GUIDE_COUNT * VERTICES * 4

var _integration_spirv: PackedByteArray
var _follower_spirv: PackedByteArray
var _rd: RenderingDevice
var _rids: Array[RID] = []
var _complete := false


func _initialize() -> void:
    call_deferred("_begin")


func _fail(message: String) -> void:
    if _complete:
        return
    _complete = true
    push_error("LoxFX renderer-owned compute: " + message)
    quit(1)


func _begin() -> void:
    _integration_spirv = FileAccess.get_file_as_bytes(INTEGRATION)
    _follower_spirv = FileAccess.get_file_as_bytes(FOLLOWERS)
    if _integration_spirv.is_empty() or _follower_spirv.is_empty():
        _fail("Required original LoxFX HLSL-to-SPIR-V shaders are missing")
        return
    RenderingServer.call_on_render_thread(Callable(self, "_record_on_renderer"))
    for i in range(480):
        if _complete:
            return
        await process_frame
    _fail("Renderer callback or async GPU readback did not complete")


func _cleanup_renderer() -> void:
    if _rd != null:
        # Pipelines and descriptor sets depend on shaders and buffers.
        # Release in inverse creation order after both GPU readbacks.
        for index in range(_rids.size() - 1, -1, -1):
            var rid: RID = _rids[index]
            if rid.is_valid():
                _rd.free_rid(rid)
    _rids.clear()


func _renderer_error(message: String) -> void:
    _cleanup_renderer()
    call_deferred("_fail", message)


func _make_shader(code: PackedByteArray) -> RID:
    var source := RDShaderSPIRV.new()
    source.set_stage_bytecode(RenderingDevice.SHADER_STAGE_COMPUTE, code)
    var shader: RID = _rd.shader_create_from_spirv(source)
    if shader.is_valid():
        _rids.append(shader)
    return shader


func _make_storage(data: PackedByteArray) -> RID:
    var buffer: RID = _rd.storage_buffer_create(data.size(), data)
    if buffer.is_valid():
        _rids.append(buffer)
    return buffer


func _set_uniforms(shader: RID, buffers: Dictionary) -> RID:
    var uniforms: Array[RDUniform] = []
    for binding in buffers.keys():
        var entry := RDUniform.new()
        entry.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
        entry.binding = int(binding)
        entry.add_id(buffers[binding])
        uniforms.append(entry)
    var set_rid: RID = _rd.uniform_set_create(uniforms, shader, 0)
    if set_rid.is_valid():
        _rids.append(set_rid)
    return set_rid


func _integration_constants() -> PackedByteArray:
    var data := PackedByteArray()
    data.resize(48)
    data.encode_u32(0, VERTICES)
    data.encode_u32(4, 0) # no followers in initial first-stage proof
    data.encode_u32(8, GUIDE_COUNT)
    data.encode_u32(12, 0)
    data.encode_float(16, 1.0 / 60.0)
    data.encode_float(20, 9.81)
    data.encode_float(24, 0.0)
    data.encode_float(28, 0.0)
    data.encode_float(32, 0.0)
    data.encode_float(36, 0.25) # translated scalp root
    data.encode_float(40, 0.0)
    data.encode_float(44, 0.0)
    return data


func _follower_constants() -> PackedByteArray:
    var data := PackedByteArray()
    data.resize(16)
    data.encode_u32(0, VERTICES)
    data.encode_u32(4, 0)
    data.encode_u32(8, GUIDE_COUNT)
    data.encode_u32(12, 0)
    return data


func _record_on_renderer() -> void:
    if not RenderingServer.is_on_render_thread():
        _renderer_error("Compute did not execute on Godot's render thread")
        return
    _rd = RenderingServer.get_rendering_device()
    if _rd == null:
        _renderer_error("No renderer-owned RenderingDevice: require Forward+ Vulkan")
        return

    var authored := PackedFloat32Array()
    authored.resize(BUFFER_FLOATS)
    for guide in range(GUIDE_COUNT):
        for vertex in range(VERTICES):
            var base: int = (guide * VERTICES + vertex) * 4
            authored[base] = guide * 0.2
            authored[base + 1] = vertex * 0.3
            authored[base + 2] = 0.0
            authored[base + 3] = 0.0 if vertex == 0 else 1.0

    var geometry := authored.to_byte_array()
    var roots := PackedByteArray()
    roots.resize(GUIDE_COUNT * 2 * 16)
    var static_guides := PackedByteArray()
    static_guides.resize(GUIDE_COUNT * VERTICES * 4)
    var tangents := PackedByteArray()
    tangents.resize(geometry.size())
    var integration: RID = _make_shader(_integration_spirv)
    var follower: RID = _make_shader(_follower_spirv)
    if not integration.is_valid() or not follower.is_valid():
        _renderer_error("Could not compile actual LoxFX SPIR-V through Godot")
        return
    var current: RID = _make_storage(geometry)
    var previous: RID = _make_storage(geometry)
    var rest: RID = _make_storage(geometry)
    var root_history: RID = _make_storage(roots)
    var guide_static: RID = _make_storage(static_guides)
    var tangent_buffer: RID = _make_storage(tangents)
    if (not current.is_valid() or not previous.is_valid()
            or not rest.is_valid() or not root_history.is_valid()
            or not guide_static.is_valid() or not tangent_buffer.is_valid()):
        _renderer_error("Could not allocate Godot-owned hair storage buffers")
        return
    var first_set: RID = _set_uniforms(integration, {
        0: current, 1: previous, 2: rest, 3: root_history
    })
    var last_set: RID = _set_uniforms(follower, {
        0: current, 4: rest, 5: guide_static, 6: tangent_buffer
    })
    var first_pipeline: RID = _rd.compute_pipeline_create(integration)
    var last_pipeline: RID = _rd.compute_pipeline_create(follower)
    for rid in [first_pipeline, last_pipeline]:
        if rid.is_valid():
            _rids.append(rid)
    if (not first_set.is_valid() or not last_set.is_valid()
            or not first_pipeline.is_valid() or not last_pipeline.is_valid()):
        _renderer_error("Godot renderer rejected LoxFX shader bindings or pipeline")
        return

    # These commands enter Godot's OWN draw graph and scheduler.
    # The second real LoxFX stage consumes the first stage's current buffer.
    var command: int = _rd.compute_list_begin()
    _rd.compute_list_bind_compute_pipeline(command, first_pipeline)
    _rd.compute_list_bind_uniform_set(command, first_set, 0)
    _rd.compute_list_set_push_constant(command, _integration_constants(), 48)
    _rd.compute_list_dispatch(command, 1, 1, 1)
    # Godot 4.7's explicit compute-list boundary flushes the producer
    # usages into its render graph before follower/tangent consumption.
    # Simply rebinding a pipeline is not a guaranteed memory dependency.
    _rd.compute_list_add_barrier(command)
    _rd.compute_list_bind_compute_pipeline(command, last_pipeline)
    _rd.compute_list_bind_uniform_set(command, last_set, 0)
    _rd.compute_list_set_push_constant(command, _follower_constants(), 16)
    _rd.compute_list_dispatch(command, 1, 1, 1)
    _rd.compute_list_end()

    # NUMERICAL TEST ONLY: one asynchronous readback after dispatch, not
    # a frame-time CPU geometry transfer or visual rendering strategy.
    if _rd.buffer_get_data_async(current, Callable(self, "_on_positions")) != OK:
        _renderer_error("Could not queue numerical integration readback")
        return
    if _rd.buffer_get_data_async(tangent_buffer, Callable(self, "_on_tangents")) != OK:
        _renderer_error("Could not queue numerical tangent readback")
        return


var _positions := PackedByteArray()
var _tangents := PackedByteArray()


func _on_positions(bytes: PackedByteArray) -> void:
    _positions = bytes
    _check_results()


func _on_tangents(bytes: PackedByteArray) -> void:
    _tangents = bytes
    _check_results()


func _check_results() -> void:
    if _positions.is_empty() or _tangents.is_empty():
        return
    if _positions.size() != BUFFER_FLOATS * 4 or _tangents.size() != BUFFER_FLOATS * 4:
        _renderer_error("Unexpected GPU result sizes")
        return
    var root_x: float = _positions.decode_float(0)
    var root_y: float = _positions.decode_float(4)
    var free_y: float = _positions.decode_float(20)
    var tail := Vector3(_positions.decode_float(16),
                        _positions.decode_float(20),
                        _positions.decode_float(24))
    var root := Vector3(root_x, root_y, _positions.decode_float(8))
    var measured_tangent := Vector3(_tangents.decode_float(0),
                                    _tangents.decode_float(4),
                                    _tangents.decode_float(8))
    if (absf(root_x - 0.25) > 0.001 or absf(root_y) > 0.001
            or absf(free_y - (0.3 - 9.81 / 3600.0)) > 0.002):
        _renderer_error("Original LoxFX integration shader produced incorrect motion")
        return
    if measured_tangent.distance_to((tail - root).normalized()) > 0.003:
        _renderer_error("Original LoxFX follower shader did not consume GPU integration output")
        return
    _cleanup_renderer()
    call_deferred("_succeed")


func _succeed() -> void:
    if _complete:
        return
    _complete = true
    print("PASS: Godot renderer-owned Vulkan executed TWO original LoxFX shaders, ",
          "GPU producer/consumer dependency, no external submit or frame readback")
    quit(0)
