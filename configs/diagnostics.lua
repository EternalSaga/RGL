-- configs/diagnostics.lua — 诊断开关（全可选，缺文件/缺段 = 全关，正常路径零影响）
-- 历史排查（4K 黑屏 UAF / 可见窗 dmabuf fault / sRGB 嫌疑）留下的门，2026-09-08 由
-- 环境变量转本配置。启动日志会打一行 "diagnostics: ..." 汇总当前值。
return {
    no_vvl = false,             -- 关 VVL（descriptor heap wrapper 双写排查）
    no_hdr = false,             -- 强制 SDR surface 格式（HDR 后缓冲 fault 排查）
    no_present = false,         -- 渲染但永不 present（KWin dmabuf import 排查）
    vtx_canary = false,         -- 顶点上传读回金丝雀（拼接 buffer 保真度）
    texel_canary = -1,          -- 帧 N 重读白纹理比对（GPU 侧 UAF 签名；-1/缺省 = 关）
    dump_gbuf = -1,             -- deferred 截图直读第 N 个 G-buffer 通道出 PNG（0..4；-1 = 关）
    style_dist = false,         -- per-primitive 风格分布日志
    tex_dist = false,           -- per-primitive 纹理 slot 分布日志
    texture_force_size = 0,     -- 纹理近邻缩放强制 N×N（尺寸阈值实验；0 = 关）
    texture_force_white = false,-- 纹理全白填充（"均匀内容不 fault"假设）
    texture_force_unorm = false,-- 纹理上传强制 UNORM（sRGB 嫌疑隔离）
}
