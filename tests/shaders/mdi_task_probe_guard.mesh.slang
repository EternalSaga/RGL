// mdi_task_probe_guard.mesh.slang —— 毒化检测器（docs/mdi_plan.md §19.12）
//
// 8060S 实测的 RADV 失效形态：同一次提交里，只要有一枚 mesh 入口**声明了
// SV_DrawIndex**，其后所有 vkCmdDrawMeshTasksEXT 直接 draw **不产出任何像素**。
// 本 shader 就是"其后的那枚直接 draw"：它**不声明 DrawIndex、不读 payload、不读任何
// 资源**，只在**固定守卫列**（col 12）画一个固定值——若 task-MDI 在前执行后它没画出
// 像素，用例直接红（列 12 保持 clear 哨兵 0）。
//
// 与 §19.9 二分矩阵里"门开 + 不发 MDI 命令"臂同型，但那臂要改引擎代码；这里把它
// 压成一次提交内两枚管线的自证结构：
//   · task-MDI 那半的锚 = 列 0..11 的 tag 值（见 .task/.mesh 两个文件）
//   · 守卫那半的锚 = 列 12 恰为 kGuardValue（0 或错位都红）
// 两半同源自证，坏法可分辨：payload 路坏 → 数据列错而守卫列对；毒化 → 守卫列丢。

struct Vert
{
    float4 position : SV_Position;
    nointerpolation uint value : TEXCOORD0;
};

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void main(
    out vertices Vert verts[4],
    out indices uint3 triangles[2])
{
    SetMeshOutputCounts(4, 2);

    // 与数据列同一套 band 数学（RT 宽 16 三方手工同源），画在守卫列上
    const float band = 2.0f / 16.0f;
    const float x0 = 12.0f * band - 1.0f;
    const float x1 = x0 + band;

    verts[0].position = float4(x0, -1.0f, 0.5f, 1.0f);
    verts[1].position = float4(x1, -1.0f, 0.5f, 1.0f);
    verts[2].position = float4(x1,  1.0f, 0.5f, 1.0f);
    verts[3].position = float4(x0,  1.0f, 0.5f, 1.0f);
    for (uint i = 0u; i < 4u; ++i)
        verts[i].value = 48879u;   // 0xBEEF（哨兵 0 之外、tag 段 1000+ 之外的第三个可辨值）

    triangles[0] = uint3(0u, 1u, 2u);
    triangles[1] = uint3(0u, 2u, 3u);
}
