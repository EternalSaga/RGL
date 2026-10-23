#version 460
#extension GL_EXT_mesh_shader : enable
// CTS 形状：DispatchMesh/Emit 组数 = 管线期常量（specialization constant）
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(constant_id = 0) const uint GROUPS_PER_TASK = 1u;
struct TaskPayload { uvec3 taskID; };
taskPayloadSharedEXT TaskPayload tp;
void main() {
    // MDI 下行身份 = gl_DrawID（task 的 WorkGroupID 每行从 0 起，与引擎读 SV_DrawIndex 同理）
    tp.taskID = uvec3(gl_WorkGroupID.x + gl_DrawID * 4099u, gl_WorkGroupID.y, gl_WorkGroupID.z);
    EmitMeshTasksEXT(1u, 1u, GROUPS_PER_TASK);
}
