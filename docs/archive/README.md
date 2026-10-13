# docs/archive — 归档文档索引

> **状态**：现行（归档索引；**归档规则**：Phase 实施计划（完成即归档）、设计规划（已实现并演进）、专题调研（结论已吸收
> 进现行文档/代码）移入本目录。归档 = 时点快照：正文中的行号/代码引用已漂移，**只取设计思路
> 与处置结论**，现状权威记录以 [AGENTS.md](../AGENTS.md) + [next_roadmap.md](../next_roadmap.md)
> 为准。每篇文档头部带状态行（`历史`/`调研`）+ 必要的勘误注。

## 索引

| 文档 | 原状态 | 一句话 | 勘误/推翻记录 |
|---|---|---|---|
| [phase2_plan.md](phase2_plan.md) | 历史 | Phase 2 glTF 加载实施计划（2026-08-23 完成） | 无 |
| [phase2_coupling_review.md](phase2_coupling_review.md) | 历史 | Phase 2 耦合审查问题清单（处置权威在 ecs_compliance_review §处置记录） | 无 |
| [phase3_plan.md](phase3_plan.md) | 已完成 | Phase 3 纹理管理实施计划（2026-08-28 完成；M3 分离绑定定案） | 无 |
| [phase4_plan.md](phase4_plan.md) | 已完成 | Phase 4 反射扩展 + 多 shader 实施计划（2026-08-28 四批次 + 08-30 收尾） | **cottage artifact"驱动竞态"定性已被批次 2 定案注记（2026-08-30）重写**：真根因 = gltf_loader uv 步长越界 + model.mesh.slang triangles 索引越界（两个共享代码 bug） |
| [phase5_plan.md](phase5_plan.md) | 已完成 | Phase 5 透明排序 + 双光照路线实施计划与记录（2026-08-31 完成，含像素级验收矩阵） | 无 |
| [phase5_5_plan.md](phase5_5_plan.md) | 已完成 | Phase 5.5 简化版 IBL + 天空盒实施计划与记录（2026-09-01 完成，含 nightSky/rainbow 面序定案表、linear staging 三坑） | 无 |
| [phase6_record.md](phase6_record.md) | 历史 | Phase 6 渲染图化实施记录（2026-09-03 完成；六条偏离注记 + 5 批次 + 执行器边界定案），自 next_roadmap 瘦身迁入 | 无 |
| [phase7_plan.md](phase7_plan.md) | 进行中→归档 | Phase 7 延迟渲染实施计划（2026-09-07 起草；批次拆解、范围决策 D1–D6、验证矩阵）——批次 5=7b 未开工，计划面仍有效 | 进度以 next_roadmap Phase 7 表 + phase7_devlog 为准 |
| [phase10_plan.md](phase10_plan.md) | 已完成 | Phase 10 调试交互实施计划（2026-09-27 立、09-28 批 10.1–10.6 全落地：imgui 骨架/ObjectDraw 单源化/参数注册表/材质控件/配置热重载/shader 热重载 L1/GPU id 点选/反射语义扩产；决策表 D1–D12） | 遗留：UI 视觉走查（用户）+ **D12 RPC 通道 = 延后**（翻案条件见 §2 决策表）；阴影已顺移 Phase 11 |
| [shader_reflection_plan.md](shader_reflection_plan.md) | 历史 | Phase 0 反射设计规划（实现现状 = tools/ 三模块 + 5 条反射管线，见 AGENTS.md） | 正文与现状代码不一致处以后者为准 |
| [heap_texture_research.md](heap_texture_research.md) | 调研 | descriptor heap 纹理采样预研（三驱动 × 5 变体全 PASS）+ UAF 排查全过程 | **§八"RADV 只映射组合绑定"被 §九 推翻**（标量 Texture2D<float> splat）；**§十"4K 黑屏 = high VA 56 位定性"被 §12.6 改写**（同根 TexturePool UAF 复用）；§十一 状态漂移同根 |
| [min_mesh_crash_已删除.md](min_mesh_crash_已删除.md) | 历史 | `min_mesh_crash/` 子项目归档墓碑（`516cf5a` 删除；根因结论 = issue_history 2026-08-17 条目 + nvidia_compatibility_report §十，代码 = git history） | 无 |
| [heap_texture_spike_已删除.md](heap_texture_spike_已删除.md) | 历史 | `heap_texture_spike/` 子项目归档墓碑（`516cf5a` 删除；结论 = heap_texture_research.md，代码 = git history） | 无 |
| [ecs_compliance_review.md](ecs_compliance_review.md) | 历史 | ECS 五条设计规范（现行摘要在 AGENTS.md）+ 2026-08-24 合规审查 + 逐条处置记录 | 五项 ECS 债 2026-08-30 全清（处置记录为时点快照） |
| [present_timing_research.md](present_timing_research.md) | 调研 | VK_EXT_present_timing 预研（决策 = 暂不引入；结论已吸收进 技术概念.md Present Mode 节） | 真实帧 pacing 需求出现时按第六节步骤执行 |
| [教程拆解发现_20260902.md](教程拆解发现_20260902.md) | 已修复 | 兄弟教程双实现对账查出 5 项（GGX epsilon/mesh 减法/契约检查/ambient/intensity 语义），2026-09-03 全部落地 | 各修复已附代码注释指针（4 处 shader 引本文件 archive 路径） |
| [light_entity_plan.md](light_entity_plan.md) | 历史 | 灯 entity 化实施计划（ECS 规范 3，2026-09-10 完成，BIT-IDENTICAL） | 正文冒烟命令曾含 `RADV_EXPERIMENTAL`，Mesa ≥26.2.3 已不需要（2026-09-24） |
| [radv_static_dgc_spike.md](radv_static_dgc_spike.md) | 历史 | RADV 静态 shader DGC spike（2026-09-10 完结：功能验证通过、满槽实现性能净亏 1.85× → 定案默认不开，`dgc_static` 门保留；紧凑分桶翻盘时以本文为设计底稿） | 无 |
| [slang_api_reflection_research.md](slang_api_reflection_research.md) | 调研 | Slang C API 直接反射调研 + 实施记录（2026-09-10 落地，`reflection_tool slang` 替代 slangc CLI→JSON 中间层，slang 管线已全切，真消除 C++↔shader 双源） | 2026-10-14 清账：原标「现行」与归档规则（archive 头部只带 `历史`/`调研`）自相矛盾，已改标调研；原文"5 条管线"数字已漂，改指 `cmake/RglReflection.cmake` |
| [架构债务-Resources职责拆分.md](架构债务-Resources职责拆分.md) | 已完成 | Resources 职责拆分（2026-09-19 完成：SceneData/RenderTargetCache/Environment/SamplerLibrary/AssetTable 拆出，allocSlot 三段化） | 无 |
| [架构债务-依赖方向审计.md](架构债务-依赖方向审计.md) | 已完成 | 依赖方向审计（2026-09-08：债务 1 方案 B + 债务 2 已修，债务 3 缓行——`vk_resources` 不添第四种角色） | 无 |
| [静态审查升级路线.md](静态审查升级路线.md) | 调研 | LLVM 23 + `-Wlifetime-safety` 两阶段落地计划（2026-09-24 定稿，尚未实施，等系统 LLVM 23；改 `tools/checks` clangbind 前先读） | 2026-10-14 清账：原标「现行」与归档规则自相矛盾，已改标调研；原文"预计 ~20 天内"失效（本机 clang 仍 22.1.8），头部已删去该排期 |
| [阴影失效排查.md](阴影失效排查.md) | 已解决 | Phase 11 批 11.2 阴影失效排查交接日志（2026-09-29 同日收口；权威结论 = 调试历史 2026-09-29 条目，本文留档排查过程与哨兵定位手段） | 无 |

## 已解决问题总账

已解决的已知问题（含根因/误诊复盘指引）见 [issue_history.md](issue_history.md)。
AGENTS.md「已知问题」节只保留**未解决**项。
