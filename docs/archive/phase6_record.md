# Phase 6 实施记录（渲染意图图化）——归档

> **状态**：历史（Phase 6 已完成 2026-09-03，5 批次；本文自 next_roadmap.md 瘦身时迁入，
> 承接原"偏离注记 + 完成情况 + 执行器边界"实施记录。原计划文档未单独归档，本文兼作权威实施记录。）

## 计划-实现偏离注记（实现为准）

- **barrier 自动推导纳入本期**（原计划未列，用户拍板"至少得有个雏形"）：layout/stage/access
  从 pass 声明的 usage 推导（`render_graph.cppm` 头注释规则表 = 单一真相源），drawFrame
  手写布局转换清零。业界定位 = Frostbite 第一代全自动图保留核心一条（自动 barrier），
  aliasing/pass fusion/多线程录制明确不做（GDC 2022 "The Future of Render Layers" 的
  API-tight 收敛方向，本就不需要全自动）。
- **Pass 拆分 render pass 实例**：原三段共享一个 beginRendering → 每 pass 独立实例，
  跨实例 LOAD 靠编译图对同布局 attachment 也发 identity barrier（写→写转换）保证执行
  依赖；中间 pass 的 color/depth storeOp 必须 store（编译器校验"后续使用的写不得
  DontCare"）。像素结果与单实例版 BIT-IDENTICAL（headless swapchain 回读 sha 对账）。
- **RGL_OFFSCREEN DIAG 转正为图变体**（批次 4）：offscreen attachment（scene_rt，
  format 暂恒 inherit=swapchain 格式）+ composite blit pass；旧手工四件套
  （offscreenImage/offscreenLayout 追踪/④⑫分支/waitStage 特判）全部退役为图机制。
  顺带修 VVL 实锤的潜伏违例：swapchain imageUsage 缺 TRANSFER_DST（blit dst 必需）。
- **Lua 面 = 纯声明数据**（sol2 沙箱，同 camera/scene_config 模式），零 handle 零
  每帧回调零热重载——启动期 load→compile→fail-fast，录制期零图错误；pipeline/state/
  attachment 全名字字符串引用。
- **entities 列表化**（批次 5）：type=model|cube + position/rotation(欧拉度)/scale
  (均匀)；缺段由旧 model/cube 字段兼容合成（行为逐位不变）。多模型暴露 heap 布局
  契约的时序强制：全部模型顶点槽先于任何纹理槽（A1/A2 两阶段循环，交错 = 契约
  断言启动拦截）。
- **自检守卫先抓到真 bug**：`renderGraphSelfCheck()`（无 GPU，main 启动期跑）首跑
  即查出"DontCare 后仍被**读**"漏校验——教训固化进编译器测试样本。

## 完成情况（5 批次）

- [x] **批次 1 render_graph 核心模块**：`RgGraphDesc`（attachment/pass 声明）+
  `compileRenderGraph`（校验 + 布局链推导 + barrier 计划 + presentTransition +
  swapchainWaitStage 派生）+ `defaultRenderGraphDesc`（三段等价图）+ 自检；纯 C++
  零 Lua 零 entt，接口/impl 分离。
- [x] **批次 2 Renderer 消费编译图**：drawFrame ④⑤⑨ 手写段 → pass 循环盲执行
  （emitBarriers 物化 + attachmentLayouts 逐 slot 追踪表）；heap/staging barrier 留
  数据通路固定段不入图；**验收 = 旧二进制 vs 图化新二进制截图 BIT-IDENTICAL**。
- [x] **批次 3 render_graph_config**：`configs/render_graph.lua`（sol2 沙箱 + 缺文件
  回落默认 + 枚举字符串启动报错）；main 接线 + "N attachments / M passes compiled"
  观测日志；Lua 图 vs 内建默认图渲染位级一致。
- [x] **批次 4 offscreen 转正**：`Resources::ensureRtAttachment`（RT 缓存 + generation
  重建检测 + resize 布局追踪重置）+ blit pass 执行 + `offscreenRenderGraphDesc` +
  `RGL_OFFSCREEN` env 图变体；双路径 VVL 零 error + 三方截图位级一致。
- [x] **批次 5 scene.lua entities**：SceneEntityDesc + registerScene/registerCubeEntity
  rootTransform + main 三段加载流（A1 顶点/A2 纹理/B env/C 烘焙注册）；兼容合成
  diff=0 + 双模型+cube 三 entity 场景实机验证。
- [x] **批次 6 文档销账**：AGENTS.md（模块表 +2、启动流、已知问题）+ next_roadmap +
  docs/技术概念.md 渲染图小节。

## 执行器边界（批次 2 定案，现行规则）

图只管 attachment 状态同步；descriptor heap host-write barrier、scene staging copy、
heap 绑定、acquire/present 信号量与双 fence 帧槽复用留 Renderer 固定段（非 attachment
语义，入图反而失真）。

## 后续完善方向（Phase 6 明确不做）

offscreen 显式格式（Phase 7 批次 1a 已兑现）、sampleRead 描述符接线（Phase 7 D1 已兑现）、
transient aliasing、pass fusion（同附件连续 pass 合并 render pass 实例）、多线程录制、
每帧 Lua 回调/热重载——后四项仍无排期。
