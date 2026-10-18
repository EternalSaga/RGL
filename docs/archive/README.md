# docs/archive — 归档文档索引

> **状态**：现行（归档索引；**归档规则**：Phase 实施计划（完成即归档）、设计规划（已实现并演进）、专题调研（结论已吸收
> 进现行文档/代码）移入本目录。归档 = 时点快照：正文中的行号/代码引用已漂移，**只取设计思路
> 与处置结论**，现状权威记录以 [AGENTS.md](../AGENTS.md) + [next_roadmap.md](../next_roadmap.md)
> 为准。每篇文档头部带状态行（`历史`/`调研`）+ 必要的勘误注。

## 索引

| 文档 | 原状态 | 一句话 | 勘误/推翻记录 |
|---|---|---|---|
| [phase10_plan.md](phase10_plan.md) | 已完成 | Phase 10 调试交互实施计划（2026-09-27 立、09-28 批 10.1–10.6 全落地：imgui 骨架/ObjectDraw 单源化/参数注册表/材质控件/配置热重载/shader 热重载 L1/GPU id 点选/反射语义扩产；决策表 D1–D12） | 遗留：UI 视觉走查（用户）+ **D12 RPC 通道 = 延后**（翻案条件见 §2 决策表）；阴影已顺移 Phase 11 |
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
