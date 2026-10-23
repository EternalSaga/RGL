#version 460
#extension GL_EXT_mesh_shader : enable
// §26 真取数链 mesh（对照 shadow_meshlet.mesh.slang 的 MDI 支）：
//   payload 身份 → 对象区间表 → 簇条目 → Tris(u8)/Remap(u32) → 顶点池（48B 步长）。
//   数据全部合法界内（§23.4 红线：引擎就是数据全对也崩）；取回值直接进 gl_Position。
//   CHAIN=0（spec 常量）= R1 trivial 锚：payload 只参与算术、固定三角（§22 已知净的 heap 版）。
layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in; // 引擎 ★M-3T-b 的 128
layout(triangles) out;
layout(max_vertices = 255, max_primitives = 85) out;               // 引擎 3×85 契约
layout(constant_id = 0) const uint CHAIN = 1u;
layout(std430, binding = 1) readonly buffer RangeTbl { uvec2 r[]; } rangeTbl; // {base,count}
layout(std430, binding = 2) readonly buffer Clusters { uint c[]; } clusterTbl; // 44B/条=11 word
layout(std430, binding = 3) readonly buffer Tris { uint t[]; } triStream;      // u8 索引流
layout(std430, binding = 4) readonly buffer Remap { uint m[]; } remapStream;   // u32 重映射流
layout(std430, binding = 5) readonly buffer Pool { uint p[]; } vertexPool;     // 48B/顶点 @0=vec3
struct Payload { uint obj; uint cstart; uint mgc; };
taskPayloadSharedEXT Payload tpl;

// 引擎 loadU8x3 逐式（4B 对齐两字拼接；host 侧给流尾留 4B padding 保证尾字可读）
uint loadU8x3(uint byteAddr) {
    uint base = byteAddr & ~3u;
    uint r = byteAddr & 3u;
    uint w0 = triStream.t[base >> 2u];
    uint w1 = triStream.t[(base + 4u) >> 2u];
    uint lo = w0 >> (8u * r);
    uint hi = (r == 0u) ? 0u : (w1 << (32u - 8u * r));
    return (lo | hi) & 0xFFFFFFu;
}

void main() {
    uint slot = gl_WorkGroupID.x;
    uint v = gl_LocalInvocationID.x;
    if (slot >= tpl.mgc) { SetMeshOutputsEXT(0u, 0u); return; } // 债-143 钳制
    if (CHAIN == 0u) { // R1 trivial：payload 只参与算术、不派地址
        SetMeshOutputsEXT(3u, 1u);
        if (v < 3u) {
            float z = 0.5f + float(tpl.obj & 3u) * 0.01f;
            float x = float((slot + tpl.cstart) & 15u) * 0.01f - 0.5f;
            vec2 lp[3] = vec2[3](vec2(x - 0.1f, -0.1f), vec2(x + 0.1f, -0.1f), vec2(x, 0.1f));
            gl_MeshVerticesEXT[v].gl_Position = vec4(lp[v], z, 1.0);
        }
        if (v == 0u) gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0u, 1u, 2u);
        return;
    }
    uvec2 rg = rangeTbl.r[tpl.obj];                       // 第 1 级：对象区间表
    uint clusterIdx = rg.x + tpl.cstart + (slot >> 1u);   // 引擎派发契约（半区 ×2）
    uint hfx = slot & 1u;
    if (clusterIdx >= rg.x + rg.y) { SetMeshOutputsEXT(0u, 0u); return; } // 界内守卫（引擎 cBase 守卫）
    uint cb = clusterIdx * 11u;
    uint triCount = clusterTbl.c[cb + 2u];               // 第 2 级：簇条目（44B 步长）
    uint vertCount = clusterTbl.c[cb + 3u];
    uint triOff = clusterTbl.c[cb + 4u];                 // Tris 段字节偏移
    uint remapOff = clusterTbl.c[cb + 5u];               // Remap 段元素偏移
    if (triCount > 128u || vertCount > 128u) { SetMeshOutputsEXT(0u, 0u); return; } // 引擎断言②
    uint triStart = hfx * 85u;
    uint remaining = triStart >= triCount ? 0u : triCount - triStart;
    uint nTri = min(85u, remaining);
    if (nTri == 0u) { SetMeshOutputsEXT(0u, 0u); return; } // 空半区早退
    SetMeshOutputsEXT(3u * nTri, nTri);
    for (uint e = v; e < 3u * nTri; e += 128u) {
        uint local = loadU8x3(triOff + (triStart + e / 3u) * 3u); // 第 3 级：Tris u8 流
        uint corner = e % 3u;
        uint localIdx = corner == 0u ? (local & 0xFFu)
                          : (corner == 1u ? ((local >> 8u) & 0xFFu) : ((local >> 16u) & 0xFFu));
        uint g = remapStream.m[remapOff + localIdx];             // 第 4 级：Remap u32 流
        uint pb = g * 12u;                                        // 第 5 级：顶点池 48B 步长
        gl_MeshVerticesEXT[e].gl_Position = vec4(intBitsToFloat(int(vertexPool.p[pb])),
                                                 intBitsToFloat(int(vertexPool.p[pb + 1u])),
                                                 intBitsToFloat(int(vertexPool.p[pb + 2u])), 1.0);
    }
    for (uint p = v; p < nTri; p += 128u)
        gl_PrimitiveTriangleIndicesEXT[p] = uvec3(3u * p, 3u * p + 1u, 3u * p + 2u);
}
