// Phase 7b（批次 5）：GPU frustum culling compute（首版不接反射管线，ByteAddressBuffer
// 手写字节偏移——docs/技术概念.md 缓冲访问规则；C++/shader 布局契约单一真相源 =
// gpu_cull.cppm CullEntry / 序列布局，本文件字面量必须一致，scene.slang 先例）
//
// **数据分工（BIT-IDENTICAL 契约）**：序列的 execSlot/DrawArgs/meshCmd 基值由 CPU 每帧
// 写进 seq staging（mvp 同为 CPU 算、现进 scene data per-object 表——GPU 若自算 mvp，FMA 收缩序与 CPU 无位级契约，
// 叶缘光栅 ±1 LSB 不可控）；GPU 只覆盖可见性：meshCmd.x@108（groupCount 或 0 = no-op）
// 与 count（@0 保留未用（旧 visible atomicAdd 无人清零已删，2026-09-15）| @4 total
// 每帧 thread0 写，DGC sequenceCountAddress 指向 @4；可见数 = CPU 读回序列 meshCmd 位统计）。
// DGC 执行路与读回重放路消费同一序列内容（CPU args + GPU 可见性），跨平台位级一致。
//
// 矩阵约定（无行/列主序歧义）：对象矩阵按"内存 chunk j = 数学列 j"处理（C++ 侧 glm
// 列主序 + MVP 生产契约已证——Phase 7b+ 后该值进 scene data per-object 表，原 DrawArgs.mvp），
// 世界球用列组合展开（scene.slang worldPos 同款）
// ——不依赖 Slang 矩阵构造函数 / operator[] 语义。
//
// 数据布局（与 gpu_cull.cppm static_assert 同源）：
//   cullTable (binding 0)：CullEntry × N，60B 步长（启动期一次性上传，host 写）
//     @0  center vec3 | @12 radius | @16 objectSlot | @20 pipelineIndex | @24 groupCount
//     @28 totalTriangles | @32 vertexOffset | @36 baseColor vec4 | @52 roughnessMetallic vec2
//   sceneData (binding 1)：对象矩阵表 @128（mat4 列主序 × kSceneMatrixCapacity，
//     [objectSlot] = spin·world，render_list 每帧写全量——被剔对象不留陈值）
//   sequences (binding 2)：per-candidate **固定槽**（非紧凑——不可见序列 meshCmd.x=0
//     = no-op draw，顺序 = candidate 序 = CPU entity 迭代序 → 重放序与 CPU 完全一致，
//     截图 BIT-IDENTICAL 前提；紧凑化是后续优化，会破坏顺序确定性）。120B 步长：
//     @0   execSlotFrag u32（= 2×pipelineIndex，CPU 写；DGC EXECUTION_SET token 第 1 位
//          = FRAGMENT 阶段——shaderStages 位 LSB→MSB）
//     @4   execSlotMesh u32（= 2×pipelineIndex+1，CPU 写；token 第 2 位 = MESH 阶段）
//     @8   DrawArgs 4B（仅 objectIndex；CPU 写，material.slang 契约——@8..108 槽区
//          保留 100B 宽度以维持 120B 步长，余 96B 空置不读）
//     @108 meshCmd 12B（CPU 写基值 x=groupCount y=z=1；**GPU 覆盖 x**）
//   count (binding 3)：@0 保留未用（旧 visible atomicAdd 无人清零已删，2026-09-15）
//     | @4 total u32（thread0 每帧写 push.sequenceCount——DGC sequenceCountAddress
//     指向此处，非紧凑 = candidate 数；可见数 = CPU 读回序列 meshCmd 位统计）
[[vk::binding(0, 0)]]
readonly ByteAddressBuffer cullTable;
[[vk::binding(1, 0)]]
readonly ByteAddressBuffer sceneData;
[[vk::binding(2, 0)]]
RWByteAddressBuffer sequences;
[[vk::binding(3, 0)]]
RWByteAddressBuffer countBuf;

struct CullPush
{
    // @0 视锥 6 面（left/right/bottom/top/near/far，归一化、法线朝内——CPU
    // extractFrustum 产物同源同值；mvp 不经 push——CPU 算进序列）
    float4 planes[6];
    // @96 candidate 数（DGC execute 的 sequence count 来源）
    uint sequenceCount;
};
[[vk::push_constant]] CullPush push;

static const uint kEntryStride = 60u;
static const uint kSceneMatrixBase = 128u;
static const uint kSequenceStride = 120u;

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    // 尾线程守卫（stream_fanout.comp.slang 同款）：dispatch 上取整 64，尾线程 i ≥
    // sequenceCount 会越界读写表/序列 buffer——robustBufferAccess 只保证"丢弃或改写
    // 绑定内存内值"，不可依赖
    if (i >= push.sequenceCount)
        return;
    uint base = i * kEntryStride;
    float3 c = asfloat(cullTable.Load3(base));
    float r = asfloat(cullTable.Load(base + 12u));
    uint objectSlot = cullTable.Load(base + 16u);
    uint groupCount = cullTable.Load(base + 24u);

    // 对象世界矩阵 M（scene data @128 + slot×64，4 列显式读——scene.slang
    // readObjectMatrix 同款，chunk j = 列 j）
    uint mb = kSceneMatrixBase + objectSlot * 64u;
    float4 m0 = asfloat(sceneData.Load4(mb));
    float4 m1 = asfloat(sceneData.Load4(mb + 16u));
    float4 m2 = asfloat(sceneData.Load4(mb + 32u));
    float4 m3 = asfloat(sceneData.Load4(mb + 48u));

    // 世界球（CPU 同式：中心 = M·(c,1) 列组合，半径 = r × max(三列长度)）
    float3 wc = (m0 * c.x + m1 * c.y + m2 * c.z + m3).xyz;
    float scale = max(length(m0.xyz), max(length(m1.xyz), length(m2.xyz)));
    float wr = r * scale;

    // 6 面测试（保守：球心到任平面距离 < −r 才剔——跨界/含内含均判可见；
    // 与 CPU sphereInFrustum 同式同值，对账基准一致）
    bool visible = true;
    for (uint p = 0u; p < 6u; ++p)
    {
        float4 pl = push.planes[p];
        if (dot(pl.xyz, wc) + pl.w < -wr)
        {
            visible = false;
            break;
        }
    }

    // GPU 只覆盖可见性（序列其余字节 = CPU 基值）：meshCmd.x@108
    sequences.Store(i * kSequenceStride + 108u, visible ? groupCount : 0u);
    if (tid.x == 0u)
        countBuf.Store(4u, push.sequenceCount);
}
