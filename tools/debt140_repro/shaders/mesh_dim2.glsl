#version 460
#extension GL_EXT_mesh_shader : enable
// 候选②补强：与 MDI 臂**不同**的 meshlet 线程维度（128 vs 3）= 派发间 SPI_SHADER_GS_MESHLET_DIM 重编程
layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;
layout(triangles) out;
layout(max_vertices = 3, max_primitives = 1) out;
void main() {
    SetMeshOutputsEXT(3u, 1u);
    uint v = gl_LocalInvocationID.x % 3u; // 128 线程都跑，3 个顶点重复写同值（维度差异才是本臂目的）
    uvec3 w = gl_WorkGroupID;
    uint h = (w.x * 73856093u) ^ (w.y * 19349663u) ^ (w.z * 83492791u);
    uint cx = h % 32u;
    uint cy = (h >> 5u) % 32u;
    vec2 lp[3] = vec2[3](vec2(0.1, 0.1), vec2(0.9, 0.1), vec2(0.5, 0.9));
    vec2 cell = vec2(float(cx), float(cy)) / 32.0;
    vec2 p = cell + lp[v] * (0.85 / 32.0);
    gl_MeshVerticesEXT[v].gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    if (gl_LocalInvocationID.x == 0u)
        gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
}
