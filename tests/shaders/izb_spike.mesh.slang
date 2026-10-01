// izb_spike.mesh.slang —— 1b 成败门：保守光栅 × mesh shader 最小验证专用（docs/hfts_izb_plan.md）
//
// 两个 workgroup 各出一根三角形（一次 draw 同时进两区，读回按 x<32 / x≥32 分区计数）：
//   wg0 = 亚像素三角：严格落在像素 (17,13) 内部、刻意不含像元中心 (17.5,13.5)——
//         非保守（像元中心采样）必 0 fragment（负向锚点），OVERESTIMATE 必 ≥1；
//   wg1 = 大三角：64×64 右下区的斜边直角三角形，三边全部避开像元中心/边界半整数
//         歧义（直角边 x=36.25/y=36.25、斜边 x+y=96.5）→ 中心采样覆盖数与
//         保守覆盖数都有精确闭式解（300 / 325，推导见测试文件头）。
// 坐标为 framebuffer 像素坐标（y 向下，左上原点），px2ndc 统一翻到 Vulkan NDC。
// 顶点色不进光栅判据（frag 恒出白），绕序无所谓（管线 cullMode=NONE，探针只测覆盖）。

struct Vert {
    float4 position : SV_Position;
};

float2 px2ndc(float2 p) {
    // 64×64：x_ndc = x/64*2-1；y 翻向（framebuffer y 向下，NDC y 向上）
    return float2(p.x / 32.0 - 1.0, 1.0 - p.y / 32.0);
}

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void main(
    in uint3 gid : SV_GroupID,
    out vertices Vert verts[3],
    out indices uint3 triangles[1])
{
    SetMeshOutputCounts(3, 1);
    triangles[0] = uint3(0, 1, 2);
    if (gid.x == 0) {
        verts[0].position = float4(px2ndc(float2(17.05, 13.05)), 0.5, 1.0);
        verts[1].position = float4(px2ndc(float2(17.45, 13.05)), 0.5, 1.0);
        verts[2].position = float4(px2ndc(float2(17.05, 13.45)), 0.5, 1.0);
    } else {
        verts[0].position = float4(px2ndc(float2(36.25, 36.25)), 0.5, 1.0);
        verts[1].position = float4(px2ndc(float2(60.25, 36.25)), 0.5, 1.0);
        verts[2].position = float4(px2ndc(float2(36.25, 60.25)), 0.5, 1.0);
    }
}
