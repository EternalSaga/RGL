# 透明混合管线（deferred 单路线下的 BLEND 落地）

> **状态：现行·进行中**（2026-10-18 立项并同日开工）。
> 上游：`docs/图变体收敛_plan.md` §9-6 的保留清单——那一笔「transparent 段与词表条目 ✓（自定义图可消费）」
> 就是本文要还的账：段还在，消费者没了。

## 0. 立项事实（2026-10-18 实测，不是推断）

删 forward 的副作用**不是**「透明排序质量下降」，是 **BLEND 图元整条不落地，且不出声**：

| 事实 | 位置 |
|---|---|
| 分桶与远→近稳定排序照旧在跑 | `render_list_impl.cppm`（`mat.alphaModeBlend ? transparent : opaque` + `stable_sort(distanceSq >)`） |
| 段边界字段照旧写 | `FrameDrawList::transparentStart` |
| 词表两档条目照旧在 | `pass_vocabulary.cppm`：`{"transparent", meshPass}` + `{"transparent", exact}` |
| **没有任何 pass 的 `drawSource = "transparent"`** | `render_graph_builtin.cppm::deferredRenderGraphDesc` 四段：geometry / light / ao / tonemap |
| ⟹ `segmentSpan("transparent", …)` 那段序列无人取用 | `frame_draw_list.cppm::segmentSpan` |

出厂场景看不出来的原因：`bistro_exterior` 的 alphaMode 分布 = 122 OPAQUE + 10 MASK + **0 BLEND**
（MASK 走 opaque 的 shader 内 discard）。而 `assets/grass_variations.glb`（4 primitive **全 BLEND**）
与 `assets/transparent.glb`（2 OPAQUE + 2 BLEND α=0.25）今天**一个像素都不出**。

## 1. 四枚裁决（2026-10-18 用户拍板）

1. **透明 pass 插在内建 deferred 图里**：`geometry → light → transparent → ao → tonemap`。
   不走「先写 `render_graph.lua` 自定义图试验」——刀②.2 §9-6 已实测自定义图起跑路通，试验不产出新信息；
   而内建图这一段就是最终形状。自定义图侧的可用性由词表条目 + `isKnownDrawSource` 天然保住。
   ⚠ **插在 `ao` 之前**这条不是审美：`ao` 把 `depth` 转成 ShaderReadOnly，透明段要 depth 做深度测试
   （只测不写），排在 ao 之后就得 SRO→DSAO→SRO 走两趟无谓转换；排在 ao 之前是
   `DSAO → DSAO → SRO` 一条链。装饰器链（taa / ao_apply / ssr / velocity）吃的是
   `swapchainColorConsumerIndex`（「写 swapchain 且 sampleRead 色附件」= tonemap），插透明段不影响它。
2. **着色核 = 抽 `shaders/pbr.slang` 共享模块**，`light.frag` 与透明 frag 同吃那一份。
   被删的 forward `model_pbr_ibl.frag` 与 `light.frag` 的关系历来是「两份实现 + 严格同源注释」
   （注释原话「逐式相同」），那正是本项目哲学第 1 条点名的形态。**先做逐位探针**（刀0）再谈别的：
   slang 对源码位置差异做 fp 重排是实测过的（2026-10-02 的 105401 像素漂移教训），
   探针判据 = `just regress ab` 逐位相同；**红了就停下来重拍，不许顺手重立基线**。
   已实测的技术前提：Slang **允许资源类型作函数参数**（`TextureCube` / `SamplerState` 传进去，
   调用方各自的 `[[vk::binding]]` 号原样保留，SPIR-V 反查确认）⟹ env 采样可以进共享模块，
   而 `light.frag`（binding 9/10）与透明 frag（binding 4/5，走 `env.slang`）不必共用绑定号。
   ⚠ **IZB 那块不搬**（`izbMask` 的 `ByteAddressBuffer` 声明 + `izbShadowFactor`）：R7 护栏对
   「新增 ByteAddressBuffer 声明」是 ERROR 级，搬家会去动那条基线；且它是 light pass 专属消费形态。
   共享模块收的是**已经算好的 shadowFactor 标量**，各调用方自己决定怎么得到它。
3. **类别变体 = 四支全做**（PBR / vegetation / light / unlit），机制照 `deferred_geometry` 那一族
   （本体 + 三个 wrapper 先 `#define MATERIAL_CLASS` 再 `#include`）。
   命名 = **transparent 族**（transparent.frag.slang 本体 + _vegetation / _light / _unlit 三个 wrapper），
   与 pass/state/段名 `transparent` 同名同义；**不叫 deferred_transparent 那种名字**——它是前向着色，
   名字里挂 deferred 会指错方向（"deferred_geometry" 那族的 deferred 指的是它写 G-buffer，本族不写）。
   🔴 **硬约束**（不是选择题）：透明 pass 只挂 `scene_hdr` 一张色附件，而 blend 图元只可能落在透明段
   ⟹ blend 图元的管线条目**必须**是单色附件的前向 frag。若某个 blend 材质被 `material_styles` /
   `deferred_styles` 连连接走 5-MRT 的 `deferred_geometry*`，那是「编译得过、VVL 才炸」的形态。
   堵法 = 风格→规格名派生里 blend 分支**排在** material_rules 连连看**之前**（顺序即护栏），
   且该派生函数从 `main`（不可单测：FILE_SET 只有可执行文件能 import）搬进 `scene_config`
   （rlbase，`just test` 可测）——把「blend 永远拿不到 G-buffer 规格」写成断言。
   代价：`grass_variations`（材质名 `Foliage` 且全 BLEND）走的是**透明侧植被变体**，
   不是 opaque 那支——所以四支全做不是贪多，是让 blend 与类别两个维度**各自独立成立**。
   风格→透明规格名新增一张 `transparent_styles` 配置表（与 `deferred_styles` 对称，
   同一批风格键），而不是给规格名加命名约定——约定 = 靠字符串猜的第二个真相源。
4. **顺带两把小刀**（同批提交，各自独立可回退）：
   - **修 highlight 锚点**：`planHighlightDecoration` 用的是 `firstColorConsumerIndex`
     （「首个 sampleRead 色附件」，**不要求写 swapchain**）。deferred 图里第一个命中的是 `light`
     ——`gbuffer_albedo` 是 `offscreen`，`isColorAttachment` 判 true ⟹ 高亮被插到 `light` 之前、
     把边缘光 alpha 混合进 **albedo G-buffer**（还发生在光照之前）。
     修法 = 改吃与 taa / ao_apply / ssr / velocity 同一条共享判据 `swapchainColorConsumerIndex`。
     **为什么以前没红**：`tests/test_render_graph.cppm` 那四条 highlight 用例喂的全是合成图
     （`opaque` 写 `scene`、`tonemap` 采 `scene` = forward 形状），**真 deferred 图一次都没喂进这个装饰器**。
   - **加「有 BLEND 却无人消费」启动出声**：场景里有 blend 图元、而图里没有任何 pass 的
     `drawSource = "transparent"` ⟹ 启动 warn（自定义图漏写那一段就出声）。这条是本次静默丢能力的直接补票。

## 2. 为什么透明只能前向着色（负知识，带失效条件）

- **作用域**：本引擎的 G-buffer 形态（一像素一套 `gbuffer_*`）+ 需要多层混合。
- **论证**：deferred 的透明只能表达「每像素最近的那一层」——第二层没有地方存它的 albedo/normal/alpha。
  而透明这件事的可见价值恰恰在多层叠加（玻璃后面的玻璃、草叶叠草叶）。
- **仍然要做的部分**：单层 deferred 透明（写 G-buffer + 第二次 light pass 混合）在「只有一层」的资产上
  比前向便宜（光照面积小），本文**不**否决它，只是不在这一刀。
- **什么实验会推翻它**：若出现「blend 图元覆盖率高、且单层占绝对多数」的真实资产，
  且前向透明在 `just cpu` 上量出可感成本，就该重开这条对照。

## 3. 施工序（四刀，逐刀可验收）

| 刀 | 内容 | 验收 |
|---|---|---|
| **0** | 抽 `pbr.slang`，`light.frag` 改 import（**搬字不改式**） | `just regress ab` **逐位相同**；红了停 |
| **1** | 透明 frag 本体 + 四支类别变体 + CMake/spec 注册 + blend 优先路由 + 内建图 transparent 段 + selfCheck 断言 + 测试手术 | `just gates` 绿；`just smoke` 出厂图逐位不变（bistro 0 BLEND ⟹ 空 pass）；`transparent.glb` 出图 |
| **2** | 两把小刀（highlight 锚点 + BLEND 无人消费出声） | 各带正例 + 反向用例；highlight 用例改喂**真 deferred 图** |
| **3** | 排序正确性判据：造一个双层 unlit 透明的合成场景，期望像素写成闭式混合式 | 变异对照：`stable_sort` 比较子 `>` → `<` **必须红** |

## 4. 已知边界（第一版不做，逐条带触发条件）

| 边界 | 现状出处 | 触发重开的条件 |
|---|---|---|
| 透明不入 GPU culling 候选 | `render_list_impl.cppm`（`candidate = !mat.alphaModeBlend`）+ `main_scene.cppm` C3 同守卫 | 透明图元数量大到 CPU 逐条录制成为 `just cpu` 可见项 |
| 透明不是阴影投影体 | `collectShadowCasters` 排 blend | 需要「玻璃投影」这类语义时（物理上多半该半透，不是不投） |
| picking / object_id 不含透明 | `replayClonedPass` 只重放 opaque 段 | imgqa 的可见集判据在带透明场景上出现系统性归属错 |
| 透明面无 velocity 写入 | `velocity.frag` 走深度重建 | 运动透明体 + TAA/FSR 拖影被真实观察到 |
| SSR 把透明内容当场景色反射；AO 拿背后不透明面的 AO 乘透明像素 | 透明段排在 `light` 后、`ao` 前（§1-1） | 交付场景出现 BLEND 资产（今天 0 个） |
| 软阴影变体（PCSS / EVSSM / IZB）不覆盖透明 frag ⟹ 透明体只有 CSM 1-tap | `softShadowVariantSpec` 只认 `light.frag*` 前缀 | 与已删 forward 同状态（forward 三个阴影 frag 也从没进变体族）；交付开 PCSS/EVSSM 且场景有 BLEND 时重开 |

## 5. 施工读数

### 刀0（2026-10-18 已落地）：抽 `pbr.slang`，逐位探针四档全绿

搬进 `rgl/shaders/pbr.slang` 的四段（**搬字不改式**，表达式与累加次序逐字保留）：
`EnvParamsL` + `readEnvParamsL`、`shadeLight`、`shadeLightVeg`、环境光项（原 main 内 ambient 块）、
直接光多灯循环（原 main 内 `shadowFactor` 之后那段）。`light.frag` 剩下 G-buffer 读取、
材质类别分支、天穹合成、IZB 消费。

**两条实测出来的技术前提**（写下来省后来者重查）：

1. **Slang 支持资源类型作函数参数**：`shadeAmbient(TextureCube<float4>, SamplerState, …)` 成立，
   且 `spirv-dis` 反查确认调用方自己的 `[[vk::binding]]` 号原样进 SPV（试验体：binding 9/10 的
   cube 与 11/12 的 2D 同时在场）。⟹ 共享着色核**一个绑定都不声明**，env 资源由调用方传入，
   `light.frag` 的 9/10 与透明 frag 的 4/5 不必统一。
2. **累加次序必须保住**：`shadeLights` 收 ambient 作初值逐灯 `color +=`，而不是「返回直接光贡献
   由调用方相加」——后者是 `(d0+d1)+d2)+ambient` 对 `((ambient+d0)+d1)+d2`，浮点结合律不成立。
   探针四档全绿说明「同序」这条做到了。

**逐位探针读数**（`just regress ab --aa off`，A = 工作树 / B = `1f01b0b`，各 3 跑）：

| 档 | 注入 | A 哈希 | B 哈希 | 判定 |
|---|---|---|---|---|
| 出厂（csm） | — | `95edc8cc` | `95edc8cc` | 逐位相同 ✅ |
| PCSS | `shadow:mode="pcss"` | `2d6ff776` | `2d6ff776` | 逐位相同 ✅ |
| EVSSM | `shadow:mode="evssm"` | `865fd1aa` | `865fd1aa` | 逐位相同 ✅ |
| IZB | `shadow:izb={on=true}` | `2316b566` | `2316b566` | 逐位相同 ✅ |

四档 = `light.frag` 的四个消费 `pbr.slang` 的 SPV 变体族（IZB×PCSS / IZB×EVSSM 组合共用同一份
`pbr` 代码路径，未单列）。同构建地板三档均 0 像素漂移。

**踩到的一条工具坑**（记一笔，别再撞）：`regress ab` 起跑前 `pgrep -x rgl` 有残留即拒跑，
而它**自己**就是 rgl 的父进程——两次 ab 叠着跑（前一次还没收口就起后一次）会让后一次
静默拿到「已有 rgl 进程在跑」的失败，而失败信息混在自检噪声里不显眼。判据 =
`pgrep -af "regress.py|[r]gl$"` 为空再起跑。
