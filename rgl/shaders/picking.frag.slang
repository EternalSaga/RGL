// Phase 10 批次 10.5：GPU id 点选 pass fragment（D3）——把本 draw 的 objectIndex+1 写进
// r32ui object-id attachment，像素级取 id；回读 1 texel = 点中物体。
//
// 接口面 = model.mesh.slang 的 ModelVertexOutput（与 deferred_geometry.frag 同款复用——
// 只声明消费 position，其余插值属性编译器按 SPIR-V 输入裁剪），push constant 复用
// DrawArgs 超集（material.slang）。绑定面：本 frag 不声明任何 descriptor——mesh 阶段的
// 0/1/3 由克隆管线条目的烘焙 mapping 透传（"映射未消费无害"方向，原 skybox 条目同先例，
// 已随刀②.2 删除）。
//
// id 值域 = objectSlot + 1（0 = 未命中：attachment clear 0 天然表达"无物体"，
// objectSlot 0 是合法槽位，+1 错开；CPU 反查侧减回）。
import model;     // ModelVertexOutput（mesh 输出接口匹配）
import material;  // push constant DrawArgs（objectIndex）

[shader("fragment")]
void main(
    ModelVertexOutput input,
    out uint outObjectIndex : SV_Target0)
{
    outObjectIndex = drawArgs.objectIndex + 1u;
}
