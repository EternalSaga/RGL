# docs/archive — 归档文档索引

> **状态**：现行（归档索引；**归档规则**：Phase 实施计划（完成即归档）、设计规划（已实现并演进）、专题调研（结论已吸收
> 进现行文档/代码）移入本目录。归档 = 时点快照：正文中的行号/代码引用已漂移，**只取设计思路
> 与处置结论**，现状权威记录以 [AGENTS.md](../AGENTS.md) 为准（原挂的 next_roadmap 本身
> 已于 2026-10-18 归档，是 Phase 0–7 历史路线图，不承担现状）。每篇文档头部带状态行（`历史`/`调研`）+ 必要的勘误注。

## 索引

| 文档 | 原状态 | 一句话 | 勘误/推翻记录 |
|---|---|---|---|
| [next_roadmap.md](next_roadmap.md) | 历史 | Phase 0–7 开发路线图（2026-10-18 归档） | 头部「下一步路由 → phase13_plan」已是历史；Phase 8 之后的实际走向在各 `phase*_plan.md`，不在本文 |
| [phase7_devlog.md](phase7_devlog.md) | 历史 | Phase 7 开发日志（D1 单灯 → D3 多灯 → culling → 重构批次；主体 2026-09-10 收尾） | 实施计划 `git:phase7_plan.md` 已删 |
| [phase8_plan.md](phase8_plan.md) | 历史 | Phase 8 Bistro 打磨（HDR / HDR env / 纹理接缝 / 灯光，2026-09-11 定案） | 无 |
| [phase9_plan.md](phase9_plan.md) | 历史 | Phase 9 性能观测 Profiler（批 9.1–9.4 全落地 2026-09-27） | 原自述「转 archive 唯一前置 = 跨机复测」——2026-10-18 用户拍板豁免；跨机读数纪律由 `docs/cpu_perf_plan.md` 承载 |
| [phase10_plan.md](phase10_plan.md) | 已完成 | Phase 10 调试交互实施计划（2026-09-27 立、09-28 批 10.1–10.6 全落地：imgui 骨架/ObjectDraw 单源化/参数注册表/材质控件/配置热重载/shader 热重载 L1/GPU id 点选/反射语义扩产；决策表 D1–D12） | 遗留：UI 视觉走查（用户）+ **D12 RPC 通道 = 延后**（翻案条件见 §2 决策表）；阴影已顺移 Phase 11 |
| [phase10b_plan.md](phase10b_plan.md) | 历史 | Phase 10b 调试交互增补（imgui 中文字体 + HTTP POST RPC，2026-09-28 全落地） | 即 phase10_plan 决策表 D12 的翻案落地 |
| [phase13_plan.md](phase13_plan.md) | 历史 | Phase 13 TAA 收口（13.1a–13.15b 批次账 + 判据方法学；Phase 14 开工即冻结） | 时域线换轨 FSR → `docs/phase14_plan.md`；§3 记债继承已被 `docs/还债总账.md` 吸收 |
| [gate_lint_plan.md](gate_lint_plan.md) | 历史 | gate_lint 诊断门立项（2026-10-02 当日收口；文末「执行偏差」） | 规则全景 = `docs/风格检查规则.md`、参数真相源 = `tools/checks/config.py`；AGENTS 仍挂号「加诊断门前看」 |
| [taa_history_space_plan.md](taa_history_space_plan.md) | 历史 | TAA 历史坐标空间改造（基空间累积，2026-10-04 落地） | 后续画质线走向 → phase13/phase14 |
| [图变体收敛_plan.md](图变体收敛_plan.md) | 历史 | 图变体收敛（删 offscreen / forward 图 → deferred 单路线，2026-10-18 落地） | deferred 单路线的维护以各源码注释为准；MDI 侧定案 = `docs/mdi_plan.md` §19.20 |
| [配置正交性_plan.md](配置正交性_plan.md) | 历史 | 渲染能力配置 × 渲染图正交化（债-129，2026-10-17 结案） | 五枚拍板 §7、施工序 §8.2；逐刀读数 = `git log -p docs/还债总账.md` |
| [heap_texture_research.md](heap_texture_research.md) | 调研 | descriptor heap 纹理采样预研（三驱动 × 5 变体全 PASS）+ UAF 排查全过程 | **§八"RADV 只映射组合绑定"被 §九 推翻**（标量 Texture2D<float> splat）；**§十"4K 黑屏 = high VA 56 位定性"被 §12.6 改写**（同根 TexturePool UAF 复用）；§十一 状态漂移同根 |
| [ecs_compliance_review.md](ecs_compliance_review.md) | 历史 | ECS 五条设计规范（现行摘要在 AGENTS.md）+ 2026-08-24 合规审查 + 逐条处置记录 | 五项 ECS 债 2026-08-30 全清（处置记录为时点快照） |
| [present_timing_research.md](present_timing_research.md) | 调研 | VK_EXT_present_timing 预研（决策 = 暂不引入；结论已吸收进 技术概念.md Present Mode 节） | 真实帧 pacing 需求出现时按第六节步骤执行 |
| [radv_static_dgc_spike.md](radv_static_dgc_spike.md) | 历史 | RADV 静态 shader DGC spike（2026-09-10 完结：功能验证通过、满槽实现性能净亏 1.85× → 定案默认不开，`dgc_static` 门保留；紧凑分桶翻盘时以本文为设计底稿） | 无 |
| [slang_api_reflection_research.md](slang_api_reflection_research.md) | 调研 | Slang C API 直接反射调研 + 实施记录（2026-09-10 落地，`reflection_tool slang` 替代 slangc CLI→JSON 中间层，slang 管线已全切，真消除 C++↔shader 双源） | 2026-10-14 清账：原标「现行」与归档规则（archive 头部只带 `历史`/`调研`）自相矛盾，已改标调研；原文"5 条管线"数字已漂，改指 `cmake/RglReflection.cmake` |
| [架构债务-依赖方向审计.md](架构债务-依赖方向审计.md) | 已完成 | 依赖方向审计（2026-09-08：债务 1 方案 B + 债务 2 已修，债务 3 缓行——`vk_resources` 不添第四种角色） | 无 |
| [静态审查升级路线.md](静态审查升级路线.md) | 调研 | LLVM 23 + `-Wlifetime-safety` 两阶段落地计划（2026-09-24 定稿，尚未实施，等系统 LLVM 23；改 `tools/checks` clangbind 前先读） | 2026-10-14 清账：原标「现行」与归档规则自相矛盾，已改标调研；原文"预计 ~20 天内"失效（本机 clang 仍 22.1.8），头部已删去该排期 |

## 已解决问题总账

已解决的已知问题（含根因/误诊复盘指引）见 [issue_history.md](issue_history.md)。
未解决项在 `docs/还债总账.md`（AGENTS.md 已不设「已知问题」节）。
