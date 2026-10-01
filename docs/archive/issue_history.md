# 已解决已知问题总账

> **状态**：现行（已解决问题总账；AGENTS.md「已知问题」节只保留**未解决**项）。
> 已解决的问题（根因、排查过程、教训）归档于此。
> 每条指向权威全文记录（同目录或现行文档）。时间倒序。
>
> **沉淀地址（批 12.3d 全量裁决）**：H1 edS3 → GUARD+TEST: features.cppm
> `depthClipDynamicSupported` 双条件门已于 12.3d-A 真实落地（此前本文件 L25
> "门改双条件"的声称与代码不符，现代码就范，矛盾收口）；H3 cottage 越界读 ①
> → TEST: tests/test_gltf_texcoord.cppm（手构 GLB 全链回归，12.3d-C）；
> H5 纹理 UAF → GUARD: texel canary 已知值台账（12.3b 落地）；
> H2/H4/H6/H7/H8 → DOC（决策记录/勘误/负知识，写不出断言，全文即归宿）。

## 2026-09-02 深夜 · edS3 depthClip "VVL 误报"结论推翻：实为 app 漏启用旧扩展 feature（已修复）

`vkCmdSetDepthClipEnableEXT` 每帧报 VVL `VUID-vkCmdSetDepthClipEnableEXT-depthClipEnable-07451`
（"the depthClipEnable feature was not enabled"）。早前（§十二 初版记录）定性为
"VVL 1.4.357 gap、app 侧无 bug、保留现状"——**该结论是误诊**。

**真相**：涉及**两个不同的 feature**，都含 "depthClipEnable" 子串，名字混淆导致误诊——
① `VK_EXT_extended_dynamic_state3` 的 `extendedDynamicState3DepthClipEnable`（app 已启用，
VUID-09423 靠它通过）；② 旧扩展 `VK_EXT_depth_clip_enable` 的 `depthClipEnable`（VUID-07451
检查的就是它，app **从未启用该扩展**）。edS3 只是把旧扩展的 `VkPipelineRasterizationDepthClipStateCreateInfoEXT`
静态状态"动态化"，底座 feature 仍须同时开启（本地 spec v1.4.360 `vertexpostproc.adoc:1060` +
VUL `device_features.cpp` 两字段分别解析自两个 sType 结构 + VVL `cc_cmd_buffer_dynamic.cpp:2361`
无条件查 `enabled_features.depthClipEnable`，三源一致；v1.4.359/main 均未豁免）。VVL 报错
**完全正确**，app 真违规，RADV 宽容执行才没崩。误诊根因：当初看到自己启用的 eds3 struct 名里
有 "depthClipEnable" 就认定"已启用"，实为张冠李戴。

**修复**：`features_config.json` optional 加 `VK_EXT_depth_clip_enable`（探测字段
`depthClipBase`）；`vkdevice.cppm` 条件链接其 feature struct 进 pNext 链、`features.cppm`
同步追加扩展名；`depthClipDynamic` 门改为 eds3 && base 双条件（缺一即不调，干净降级）；
`rlvk` 补 `vk::PhysicalDeviceDepthClipEnableFeaturesEXT`。验证：冒烟 `was not enabled` 报错
归零且命令仍实调（0 报错源于 base 满足而非 no-op）。
**教训**：VVL feature 报错先核对 **VUID 引用的确切 feature 结构/sType**，别被相似命名带偏；
"某个含该子串的 struct 在链里=TRUE" ≠ "被检查的那个 feature 启用了"。

## 2026-09-02 · VMA 构造实参错位误诊 RADV 扩展 bug（zero_init 已回退）

启用 `VK_EXT_zero_initialize_device_memory` 后主崩溃。曾误诊为 RADV 驱动 bug（"启用扩展就崩"
表象）。真根因 = `VmaAllocatorRAII` 构造调用**实参错位**——追加 `zeroInit` 参数时把 `false`
塞进 `flags` 位，`VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` 被清 0 → 所有 BDA buffer
创建返回 `VK_ERROR_INITIALIZATION_FAILED`。深挖证 VMA 3.4 对 BDA buffer 自动加内部
`VkMemoryAllocateFlagsInfo(DEVICE_ADDRESS)`，与用户 `pMemoryAllocateNext(ZERO_INITIALIZE)`
必撞 = spec 违例；且本 app 无收益（RADV 新分配内存本身全零 + heap 只分配一次）→ **回退**。
教训：归因外部驱动前先把"自己代码与基线的 diff"逐行审干净。
**全文**：[vulkan_extensions_2025_调研.md](../vulkan_extensions_2025_调研.md) §十二（现行，含
headless 落地 + edS3 误诊→勘误→修复全记录（§12.3 勘误））。

## 2026-08-30 · cottage 偶发 artifact = 两个共享代码越界 bug（非驱动竞态）

"非确定性驱动竞态、复现率 1/13"为误诊。真根因两处（自 Phase 2 就存在，Windows/NVIDIA
移植暴露）：① `gltf_loader.cppm` uv 越界读（`uv[idx*3+…]` 应为 `uv[idx*2+…]`，vec2 误按
vec3 步长）→ 特定区域采样为黑（741px artifact 根因）+ Windows AV 崩溃；② `model.mesh.slang`
triangles 索引用全局顶点索引（`base=(firstTriangle+ti)*3` 应为组内 `ti*3`）→ NVIDIA 几何崩形
（RADV 对越界 index 做 mod 回绕恰好掩盖，`firstTriangle*3=30·g` 恰为 30 倍数）。
教训：**"偶发 + 区域固定 + 随构建时序变" = 越界读经典签名**，应第一时间查步长/索引
（ASan/页保护），别默认"驱动 bug"接受推进。
**全文**：[phase4_plan.md](phase4_plan.md) 批次 2 定案注记（含 12+ 项排查复盘）。

## 2026-08-30 · ECS 五项已知债全清

① cube 非 entity（重构批次 3：cube 一等资产 + registerCubeEntity）；② MaterialRef 值拷贝
（重构批次 2：真引用 + MaterialAsset）；③ push constant 布局未归一（Phase 4 批次 1：统一
DrawArgs 超集 + 零 AssetKind 分支）；④ 管线表 392 条未去重（Phase 4 批次 2：组合去重
→ 12 条 / 6 heap slot）；⑤ 无统一 slot 分配器（统一 allocSlot + 契约断言）。
**全文**：[ecs_compliance_review.md](ecs_compliance_review.md) §处置记录 +
[next_roadmap.md](../next_roadmap.md) 对应 Phase 完成情况。

## 2026-08-28 · 交互模式 GPUVM fault（状态漂移 + per-process）= TexturePool 生命周期 UAF

"状态漂移/分钟级翻转/per-process"= **UAF 非确定性**：`TexturePool` 声明在 main() if 块内，
块尾析构释放纹理 BO + 描述符存储 → 第 0 帧起 GPU 采样已释放 VA；VA 未复用 = 读旧页
"正常"（好状态），被复用为权限不同 BO = TCP fault（坏状态）。2de6731 把声明提出 if 块
即修复（commit message 误标 slot 防再发教训：所有权规则 + texel 回读金丝雀）。
**全文**：[heap_texture_research.md](heap_texture_research.md) §十一 + §12.6/§12.7。

## 2026-08-28 · RADV 4K 大纹理黑屏（"驱动限制"定性推翻）

"64MB sRGB 纹理 + 渲染目标 <2048 → 全黑 / GPUVM TCP fault，根因 = 56 位 VA 截断 / 48 位
描述符编码不下"为误诊——全部观测在 UAF 修复前构建上取得（"high VA BO" = 已释放 VA 复用后
的分配）。2026-08-28 傍晚遗留验证 PASS：master 真实 4K 材质模型（cottage 11 张 4096²）+
精确配置两路复跑零 fault → **4K 黑屏 = 同根 UAF 复用**，上游 bug report 不发。
**全文**：[heap_texture_research.md](heap_texture_research.md) §十 + §12.6 勘误。

## 2026-08-17 · NVIDIA mesh shader Xid 13 崩溃 = 缺 NO_TASK_SHADER flag

"mesh shader + shader object + descriptor heap 在 NVIDIA 崩"曾定性为 NVIDIA 驱动 bug。
真根因 = 应用侧 spec 合规：mesh shader 对象创建缺 `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT`
（TASK stage 绑 NULL 时必需），NVIDIA 驱动遵守契约查 task shader 空指针 → Xid 13 → engine
reset。VVL 查不出来是 spec 漏洞（VUID 08694 需 taskShader+meshShader 双 feature）。
**全文**：[nvidia_compatibility_report.md](../nvidia_compatibility_report.md) 第五/八节 +
完整复现代码 = git history `min_mesh_crash/`（`516cf5a` 删除；墓碑 [min_mesh_crash_已删除.md](min_mesh_crash_已删除.md)）。

## 2026-08-27 · spike "RADV 只映射组合 OpTypeSampledImage" = shader 声明错误

descriptor heap 三驱动交叉测试首测"RADV 不支持分离绑定"为误诊：spike shader 把 RGBA8 纹理
误声明为标量 `Texture2D<float>`（Slang 语义 R 通道 splat 到 4 通道）+ `samplerHeapOffset`
字段误用。修正后三驱动 × 5 变体全 PASS，语义分离三驱动完整实现。
**全文**：[heap_texture_research.md](heap_texture_research.md) §八勘误注 + §九。
