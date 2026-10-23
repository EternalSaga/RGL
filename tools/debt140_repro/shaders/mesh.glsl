#version 460
#extension GL_EXT_mesh_shader : enable
// 每个 mesh 组输出 1 个三角形；heavy=0 画哈希小格（工作量证明），heavy=1 画全屏（每组执行时长↑）
layout(local_size_x = 3, local_size_y = 1, local_size_z = 1) in;
layout(triangles) out;
layout(max_vertices = 3, max_primitives = 1) out;
layout(constant_id = 0) const uint HEAVY = 0u;
layout(constant_id = 1) const uint USE_DRAWID = 0u; // M-1b 形状：mesh 读 DrawIndex
layout(constant_id = 2) const uint MESHREAD = 0u;   // 引擎形状：DI → 内存 → 内存 双间接
layout(std430, binding = 1) readonly buffer RowTable { uint t[]; } rows_;
struct TaskPayload { uvec3 taskID; };
taskPayloadSharedEXT TaskPayload tp;
void main() {
    SetMeshOutputsEXT(3u, 1u); // 必须：声明本组输出 3 顶点 1 图元
    uint v = gl_LocalInvocationID.x;
    uvec3 w = gl_WorkGroupID + tp.taskID * 4093u; // task 身份混入哈希：校验和随组数缩放
#ifdef MESHDI
    if (USE_DRAWID != 0u)
        w.x += gl_DrawID * 65537u; // M-1b：mesh 入口声明 DrawIndex 且进真实数据流
    if (MESHREAD != 0u) {
        // 引擎形状：DI → 行表 load → 身份 → 二次 load（坏 DI 变垃圾地址 = fault 通路）
        uint r = rows_.t[gl_DrawID % rows_.t.length()];
        uint v = rows_.t[r % rows_.t.length()];
        w.x += v;
    }
#endif
    vec2 p;
    if (HEAVY != 0u) {
        vec2 big[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
        p = (big[v] + 1.0) * 0.5;
    } else {
        uint h = (w.x * 73856093u) ^ (w.y * 19349663u) ^ (w.z * 83492791u);
        uint cx = h % 32u;
        uint cy = (h >> 5u) % 32u;
        vec2 lp[3] = vec2[3](vec2(0.1, 0.1), vec2(0.9, 0.1), vec2(0.5, 0.9));
        vec2 cell = vec2(float(cx), float(cy)) / 32.0;
        p = cell + lp[v] * (0.85 / 32.0);
    }
    gl_MeshVerticesEXT[v].gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    if (v == 0u)
        gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
}
