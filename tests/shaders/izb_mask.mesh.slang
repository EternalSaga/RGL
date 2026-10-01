// izb_mask.mesh.slang —— 批 11.3-1d：光空间遮挡三角发射（FTIZB pass5 的 spike 侧）
//
// 一个 workgroup 发一个遮挡三角（drawMeshTasksEXT(triCount,1,1)），几何从 params2
// SSBO 读（**单一几何源**：测试写表、mesh 投影、frag 用同表做 edge test——不存在
// "推导抄一份、着色器再抄一份"的双源）。
//
// 坐标约定（1b 实测钉死，1d 集成必须一致）：光空间 uv ∈ [0,1]²，v=0 贴 framebuffer
// 第 0 行；NDC y = 1 − 2v + **管线负高度 viewport**（引擎 applyDynamicState 同款，
// 全引擎唯一一次 y 补偿——正 viewport 会镜像）。
//
// params2 SSBO 字节布局（唯一声明点，与 izb_mask.frag.slang 共用）：
//   [0] int triCount
//   [4] int gridDim（光空间网格边长 G）
//   [16 + 64*t + 0,4,8,12,16,20]  float 三角 t 的 v0.xy v1.xy v2.xy（光空间 uv）
//   [16 + 64*t + 24,28,32]        float 三角 t 三顶点的光深度（插值属性）

[[vk::binding(7)]] ByteAddressBuffer params2;

struct Vert {
    float4 position : SV_Position;
    float depth;                     // 光深度（插值；frag 与节点深度比较）
    nointerpolation uint prim;       // 三角序号 = mask 位（平摊属性）
};

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void main(
    in uint3 gid : SV_GroupID,
    out vertices Vert verts[3],
    out indices uint3 triangles[1])
{
    const uint t = gid.x;
    if (t >= uint(params2.Load(0))) {  // 防御：越界 wg 出空（triCount 是唯一真相）
        SetMeshOutputCounts(0, 0);
        return;
    }
    SetMeshOutputCounts(3, 1);
    triangles[0] = uint3(0, 1, 2);

    const int base = 16 + 64 * int(t);
    const float2 v0 = asfloat(params2.Load2(base + 0));
    const float2 v1 = asfloat(params2.Load2(base + 8));
    const float2 v2 = asfloat(params2.Load2(base + 16));
    verts[0].position = float4(v0.x * 2.0 - 1.0, 1.0 - 2.0 * v0.y, 0.5, 1.0);
    verts[1].position = float4(v1.x * 2.0 - 1.0, 1.0 - 2.0 * v1.y, 0.5, 1.0);
    verts[2].position = float4(v2.x * 2.0 - 1.0, 1.0 - 2.0 * v2.y, 0.5, 1.0);
    verts[0].depth = asfloat(params2.Load(base + 24));
    verts[1].depth = asfloat(params2.Load(base + 28));
    verts[2].depth = asfloat(params2.Load(base + 32));
    verts[0].prim = t;
    verts[1].prim = t;
    verts[2].prim = t;
}
