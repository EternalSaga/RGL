#version 460
#extension GL_EXT_mesh_shader : enable
// 引擎形状：DispatchMesh 组数 = 数据驱动（storage buffer 逐行读，§21 头号未测维）
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430, binding = 0) buffer XSrc { uint x[]; } xsrc;
struct TaskPayload { uvec3 taskID; };
taskPayloadSharedEXT TaskPayload tp;
void main() {
    uint lin = gl_WorkGroupID.x + gl_WorkGroupID.y * gl_NumWorkGroups.x
             + gl_WorkGroupID.z * gl_NumWorkGroups.x * gl_NumWorkGroups.y;
    uint g = gl_DrawID; // direct 模式恒 0；MDI = 行号（引擎的 SV_DrawIndex）
    tp.taskID = uvec3(gl_WorkGroupID.x + g * 4099u, gl_WorkGroupID.y, gl_WorkGroupID.z);
    EmitMeshTasksEXT(1u, 1u, xsrc.x[(g * gl_NumWorkGroups.x * gl_NumWorkGroups.y * gl_NumWorkGroups.z + lin) % xsrc.x.length()]);
}
