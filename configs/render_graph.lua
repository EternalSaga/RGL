-- 渲染意图图（Phase 6，configs/render_graph.lua）
-- 声明 attachment/pass 拓扑，C++ 编译为可执行序列（barrier/布局转换自动推导）。
-- 本文件缺失或整段缺省 = 回落默认三段图（与下方内容等价）。结构规则与字段语义
-- 权威记录：vulkan_practice/render_graph.cppm（类型契约）+ render_graph_config.cppm
-- （Lua 格式）；加载 = sol2 沙箱（禁 dofile/require），枚举字符串写错 = 启动报错。
--
-- attachments: kind = "swapchain" | "depth" | "offscreen"（帧首丢弃规则：
--   swapchain/depth 每帧内容按 load 重建；offscreen 持久图跨帧追踪布局）
-- passes:      线性序 = 执行序；kind = "draw"（默认，画 FrameDrawList 段）| "blit"
--   state = 动态状态名（"opaque"/"skybox"/"transparent"），draws = 绘制段名
--   outputs/inputs 条目: usage = colorWrite | depthWrite | sampleRead |
--   transferRead | transferWrite；load = "clear"|"load"，store = "store"|"dontCare"；
--   clear = {r,g,b,a}（颜色）/ clearDepth = f（深度），缺省 = 0.1 灰背景 / 1.0 远平面
-- 编译器校验（读先于写、同 pass 读写冲突、后续使用的写必须 store、swapchain 必须
-- 被写……）失败即启动报错，见 compileRenderGraph。
return {
    attachments = {
        { name = "swapchain", kind = "swapchain" },
        { name = "depth", kind = "depth" },
    },
    passes = {
        { name = "opaque", state = "opaque", draws = "opaque",
          outputs = {
              { attachment = "swapchain", usage = "colorWrite", load = "clear", store = "store",
                clear = { 0.1, 0.1, 0.1, 1.0 } },
              { attachment = "depth", usage = "depthWrite", load = "clear", store = "store",
                clearDepth = 1.0 },
          } },
        { name = "skybox", state = "skybox", draws = "skybox",
          outputs = {
              { attachment = "swapchain", usage = "colorWrite", load = "load", store = "store" },
              { attachment = "depth", usage = "depthWrite", load = "load", store = "store" },
          } },
        { name = "transparent", state = "transparent", draws = "transparent",
          outputs = {
              { attachment = "swapchain", usage = "colorWrite", load = "load", store = "store" },
              { attachment = "depth", usage = "depthWrite", load = "load", store = "dontCare" },
          } },
    },
}
