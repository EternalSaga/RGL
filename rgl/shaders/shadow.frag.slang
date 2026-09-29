// Phase 11 批 11.2：阴影 pass 的 fragment stage —— **无输出**（纯深度写入）。
//
// Vulkan 的动态渲染里 depth-only pass 仍需一个 frag stage（否则光栅化阶段不完整、
// vkCmdDrawMeshTasksEXT 的 pipeline 缺 FS 会创建失败）。本文件只提供一个空壳入口：
// 唯一输入是 SV_Position，函数体为空 = 什么都不写，深度由光栅化阶段的 depth test +
// depth write 落进 shadow_map（d32_sfloat）。
//
// **为什么不像 ao.frag 那样写 SV_Target**：depth-only pass 的颜色附件数为 0，
// 写 SV_Target 会让 shader object 的输出接口与 beginRendering 的附件集不匹配
// （执行器侧 applyDynamicState 也会因 colorAttachmentCount == 0 整段跳过色态，
// 见 shader.cppm 该参数注）。

[shader("fragment")]
void main(float4 position : SV_Position)
{
    // depth-only：不写任何颜色输出。参数仅用于满足入口签名（SV_Position 必被接收）。
}
