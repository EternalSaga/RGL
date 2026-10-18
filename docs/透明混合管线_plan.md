# 透明混合管线（deferred 单路线下的 BLEND 落地）

> **状态：现行·进行中**（2026-10-18 立项并同日开工）。
> 上游：`docs/archive/图变体收敛_plan.md` §9-6 的保留清单——那一笔「transparent 段与词表条目 ✓（自定义图可消费）」
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
| **3** | 排序正确性判据：造一个双层 unlit 透明的合成场景，期望像素写成闭式混合式 （**已落地**：合成场景 `blend_sort` + 常驻检查 `just imgqa blend-sort`，债-132 结案） | 变异对照：`stable_sort` 比较子 `>` → `<` **必须红**（实测已红，且只有 `blend-sort-order` 一条红，见 §5 刀3） |

## 4. 已知边界（第一版不做，逐条带触发条件）

| 边界 | 现状出处 | 触发重开的条件 |
|---|---|---|
| 透明不入 GPU culling 候选 | `render_list_impl.cppm`（`candidate = !mat.alphaModeBlend`）+ `main_scene.cppm` C3 同守卫 | 透明图元数量大到 CPU 逐条录制成为 `just cpu` 可见项 |
| 透明不是阴影投影体 | `collectShadowCasters` 排 blend | 需要「玻璃投影」这类语义时（物理上多半该半透，不是不投） |
| picking / object_id 不含透明 | `replayClonedPass` 只重放 opaque 段 | imgqa 的可见集判据在带透明场景上出现系统性归属错 |
| 透明面无 velocity 写入 | `velocity.frag` 走深度重建 | 运动透明体 + TAA/FSR 拖影被真实观察到 |
| SSR 把透明内容当场景色反射；AO 拿背后不透明面的 AO 乘透明像素 | 透明段排在 `light` 后、`ao` 前（§1-1） | 交付场景出现 BLEND 资产（今天 0 个） |
| 软阴影变体（PCSS / EVSSM / IZB）不覆盖透明 frag ⟹ 透明体只有 CSM 1-tap | `softShadowVariantSpec` 只认 `light.frag*` 前缀 | 与已删 forward 同状态（forward 三个阴影 frag 也从没进变体族）；交付开 PCSS/EVSSM 且场景有 BLEND 时重开 |
| 透明 frag 的 `isLamp ? 0.25` 是**照抄现役延迟行为**，不是独立判断（该 0.25 本身口径存疑：它当年补偿的 pack 强度早在 2026-09-29 被归位成 1.0） | `transparent.frag.slang` 的 `isLamp` 分支 + `light.frag.slang` 同族项 | **债-131 裁决落地时**——两处必须一起改，只改一处 = 透明/不透明玻璃自发光差 4 倍 |

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


### 刀1（2026-10-18 已落地）：透明段进内建图，出厂图逐位不变 + 透明资产出图
**落地面**：`transparent.frag.slang` 本体 + `_vegetation` / `_light` / `_unlit` 三个 wrapper
（`#define MATERIAL_CLASS` + `#include`，照 deferred_geometry 那一族）；`deferredRenderGraphDesc`
插入 `transparent` 段（scene_hdr `load+store`、depth `load+store` 借来只测不写、drawSource 吃
透明段）；`findSpecBase` 的模型 frag 回落从 4 条 if 链改成 **8 条表** `kMeshFragSpecs`
（两族各 4，表长写错 = 编译期红）；新增 `transparent_styles` 配置表 + `SceneConfig::transparentStyleMap`；
风格派生本体从 main 搬进 `scene_config::resolvePipelineSpecName`（**为了写得出手上的测试**：
main 的 FILE_SET 只有可执行文件能 import）；`env.slang` 收缩成只剩 binding 4/5 声明
（`EnvParams` / `readEnvParams` 移入 `pbr.slang`，刀0 那份额外的本地副本 `EnvParamsL` 顺势消失）。

**验收读数**：

| 判据 | 结果 |
|---|---|
| `just gates`（build → test → checks --strict） | 全绿（测试 575 例 / 96360 断言） |
| 出厂图逐位（`regress ab --aa off`，A=刀1 / B=刀0） | **逐位相同** ✅ 两侧均 `95edc8cc`（出厂默认锚），地板 0 漂移 |
| 五种图形态 VVL 冒烟（`aa=off/taa/fsr` + `ssr` + `diagnostics.picking`） | 5/5 PASS，errors=0 warnings=0 |
| 透明资产真出图 | `transparent.glb`（2 OPAQUE + 2 BLEND α=0.25）+ Phase 5 排序机位 `position={1.69,2.5,-19.46} target={0,0,0} mode="trackball"` ⟹ 混合可见、不透明盒正确遮挡混合盒、VVL 零报错 |

**变异对照实测**（改坏被测码必须让测试变红；六条全 RED，每条还原后重跑基线复绿；
harness = 四条「陈旧二进制」防呆逐条落实）：
A 族判据恒走 deferred / B 回落写死 deferred 本体 / C 族判据反向 / D 透明段 scene_hdr 由 Load 改 Clear /
E 透明段挪到 ao 之后 / F 透明段多挂一张色附件。

**三条实测发现**（都是"照记忆写会写错"的那类）：

1. **`SceneConfig` 的默认构造 ≠ 交付形态**。它的 `materialRules` / 两张风格表**没有类内初始化器**，
   默认值住 `defaultSceneConfig()`；而 `DiagnosticsConfig` / `ShadowConfig` 是"结构体默认 = 交付形态"
   （gate_lint 的总锚就靠那条）。同一个仓库里两种惯例并存，按 `const DiagnosticsConfig d;` 的手感
   写 `const SceneConfig d;` 会得到三张空表 + 全部断言走回落。
   `TEST: tests/test_scene_config.cppm`（用例内已就地记这条 ⚠）。
2. **`renderGraphSelfCheck` 此前只在启动期跑**（唯一调用点 = `main_impl`）⟹「内建图应该是几段、
   透明段挂几张色附件、load 是不是 Clear」这批断言**只有有人真的把引擎跑起来才会执行**，
   `just test` 完全听不到。刀1 补一条 `CHECK(renderGraphSelfCheck().empty())` 把整批图形态断言
   搬进每次提交的必经之路（纯 CPU、毫秒级）。
   `GUARD:` 无 —— 这条只能靠"测试里调用它"，没有机制保证下一个 selfCheck 断言也被跑到。
3. **selfCheck 的样本名会撒谎**：那行 `expectOk("deferred 3-pass", …)` 在图加 ao 时没人改，
   到刀1 之前一直是"3-pass = 四段"。计数属于 `selfCheckStructure` 的断言，不属于样本名 ⟹
   改名 `expectOk("deferred 内建图", …)`（**名字里不带计数**，下次加段就无从可漂）。

**刀1 留下的一条实测症状**（刀2 已接，读数见下）：场景含 BLEND 图元**且 `scene.lua` 没有
`environment` 段** ⟹ 启动硬失败，症状是 VVL `VUID-VkShaderCreateInfoEXT-flags-11292`
（透明 frag 声明的 binding 4 没有 mapping，因为 `Environment::get()` 为空时 `addModelEntry`
不置 `hasEnv`）。方向是对的（响亮、起不来），但那条消息对不知情人等于天书。

### 刀2（2026-10-18 已落地）：修 highlight 锚点 + 两条装配期对账

**① highlight 锚点**：`planHighlightDecoration` / `applyHighlightPlan` 改吃链上共享判据
`swapchainColorConsumerIndex`（「写 swapchain 且 sampleRead 色附件」），`firstColorConsumerIndex`
随之**删除**（无剩余问者 = 不留死码）；并补上 `graphDecoratedTooLate` 早退（与 taa / velocity
同一条次序契约：高亮若插到 taa 之后，它写 scene_hdr 而 tonemap 读 taa_out ⟹ 静默白画）。

**② 装配期对账**（`main_scene::validateBlendSupport`，两条任一不过 = 抛）：有 BLEND 图元而
图里没人画透明段 / 没有 env cubemap。新增图事实谓词 `graphDrawsTransparentSegment`
（判据 = `pass.drawSource`，**不是** `state`——要问的是"有没有人画那一段"）。

**三条实测教训**：

1. **对账放错时点 = 判据从未跑过**。第一版放在 `assembleBackEnd`（C3），端到端实测**没响**：
   env 缺失的真实崩溃点在 C1 `bakePipelineEntries` 的 `vkCreateShadersEXT`。挪到
   `uploadEnvironment` 之后、C1 之前才生效。
   ⟹ 这类"启动前置检查"的时序不是排版问题，**必须端到端跑一遍确认它会响**（单测打不到 main）。
2. **判据源受时点约束**：`AssetTable` 的材质记录要到 C2 `registerScene` 才进表，所以 C1 之前的
   对账只能读 `UploadedAssets` 里的 `GltfPrimitive.alphaModeBlend`（两处字段同源，但只有后者
   在本函数时点可见）。为此先写的 `AssetTable::hasBlendMaterial()` 连同其用例一并**删掉**——
   留一个没有问者的 API 就是留一条"同一件事两种问法"的债。
3. **旧用例拦不住这个破口，是量出来的**：把锚点退化回「首个采色 pass」（= 复现线上行为）之后跑
   `--test-case="*highlight*"`：**8 条里 7 条全绿**，只有新加的那条红
   （`insertBefore` 变 `light`、写的附件变 `gbuffer_albedo`、位置错一格——三条同时中）。
   ⟹ "四条 highlight 用例喂的全是 forward 形状合成图"从推断升级为实测。

**变异对照实测**（每条只跑相关用例以便归因；还原后重跑全量基线复绿）：
G 锚点退化回「首个采色 pass」→ RED（旧 7 条同时全绿，见上）/ H 谓词读 `state` 而非 `drawSource`
→ RED / I 删「装饰太晚」早退 → RED。
⚠ G 的第一版变异（把 `writesSwapchain` 从条件里摘掉）拿到的是**编译失败**而不是判据红
（`-Wunused-variable`）——正是 skill `rgl-test-discipline` 那条「谓词类变异要保留参数使用」；
改成 `true || any_of(...)` 才量到判据本身。

**端到端三档读数**（`just smoke`，配置临时改、跑完逐字节还原核对）：
① BLEND + 无 environment 段 → 退出码 1，消息点名「没有 environment 段」✅
② BLEND + 自定义图无透明段 → 退出码 1，消息点名 `draws = "transparent"` ✅
③ 合法 BLEND 场景（transparent.glb + env + 内建图）→ PASS errors=0（**不误伤**）✅
另：`diagnostics.picking=true`（高亮真落图）VVL 冒烟 PASS errors=0；`just gates` 全绿（577 例）。

### 刀3（2026-10-18 已落地）：排序合成场景 `blend_sort`，闭式四区逐字节零误差

**先记一条差点让这份输入白造的事实**（量出来的，不是推断）：透明排序的代理键是 **entity 世界矩阵的
平移列**（`render_list_impl.cppm` 的 `tf.world[3]`），而 loader 把 `node.translation` 合成进
`prim.nodeWorld`（`gltf_loader_impl.cppm` 的 `nodeLocalTRS`）⟹ **落位必须靠 node 平移**。
`tools/qa_scenes.py` 既有七个场景一律「几何烘世界坐标 + node identity」，照那个写法造双层场景
会得到**所有图元 distanceSq 相等**的序列，而 `stable_sort` 对相等键视为已序 ⟹ 比较子 `>` 与 `<`
给出**同一个序列**，变异根本不红，而画面还可能因作者顺序恰好正确 = 一条假绿的判据。
为此 `Piece` 加了 `translation`（默认 `None` = 不写键），并让 harness 在算判据**之前**先断言
各半透层 distanceSq 互异（退化即拒跑，不靠图去猜）。

**三条设计裁决各自的理由**（都在堵"这份输入其实立不起来、却没有任何东西喊"）：

1. **三层全 unlit**：`transparent.frag` / `light.frag` 的 unlit 分支都是**早退**（不采光照/
   阴影/env、不加自发光）⟹ 闭式只剩「baseColor + 混合 + 显示链」，不必复刻 BRDF/IBL/阴影，
   也**绕开债-131** 那个未决的自发光电平（生成器另加一条闸门：BLEND + emissive 直接拒绝生成）。
2. **颜色只取纯 0/1**：透明段写 RGBA16F，0/1 与 α∈{0.5,0.25} 的混合结果全是 2 的幂的有限和
   ⟹ half 精度**逐位精确**；不透明侧 albedo 要过 G-buffer 8bit sRGB 往返，而 0/1 是那条往返的
   **不动点** ⟹ 背景值不必模型化量化。（选 0.5 这类中间值会把量化往返混进判据，红时分不清是谁。）
3. **四个判据区由几何解析算出**（不在图里找边界）：far-only / overlap / near-only / bg-only。
   前三个各锚住「显示链 + 单个 α」，于是 overlap 区新增的**唯一未知就是合成次序**。

**怎么重跑**（债-132 结案后的常驻入口，一条命令跑完「复制冻结配置 → 起引擎 → 取四区像素 → 闭式对拍」）：

```bash
just imgqa blend-sort    # 四区逐字节对拍，任一不符 rc=1；无 flag（阈值是量具的组成部分，可调 = 能调绿）
just imgqa selftest      # 闭式算式本体的自检：不启引擎、不读文件，无 GPU 的机器上照样断言那四个数
```

缺 `build/rgl/models/blend_sort.glb` 时命令**直接 FAIL 不 SKIP**，并把生成命令打在错误消息里
（`python3 tools/qa_scenes.py --out build/rgl/models --only blend_sort`）。
`just gates` 第四步 `tools-check` 会编 imgqa 并跑它的自检（债-133 已结案：本任务接线时实测
刀1 那次提交起 `imgqa selftest` 就已异常退出、其后测试一条不跑，而当时三道门禁连编三次全绿）。
但**跑这条命令本身要 GPU**，gates 只保证它编得过、算式自检过，不保证它跑过。

**闭式对拍读数**（1280×720，采样行 y=360）：

| 区 | x | 覆盖层 | 线性 HDR | 闭式字节 | 实测字节 |
|---|---|---|---|---|---|
| far_only | 509 | layer_far | (0, 0.25, 0.75) | (0,113,187) | (0,113,187) ✅ |
| overlap | 633 | far→near | (0.5, 0.125, 0.375) | (162,73,141) | (162,73,141) ✅ |
| near_only | 778 | layer_near | (0.5, 0, 0.5) | (158,0,159) | (158,0,159) ✅ |
| bg_only | 1061 | — | (0, 0, 1.0) | (0,0,204) | (0,0,204) ✅ |

**四区全部零误差**（不是 ±1 LSB 容差命中）。显示链吃 `tools/imgqa/analytic.cpp` 的 `toBytes`
（曝光 → Narkowicz ACES → sRGB 编码）——它是 shader 那条链的**独立第二实现**，不是引擎代码；
独立实现在不在这里红不靠注释：三个单层区各锚一次，而 `imgqa selftest` 把「`toBytes` 吃这四个
线性值必须给出这四组字节」钉成了断言（有人改 ACES 常数会先在 selftest 红，那时 GPU 都不用跑）。

**变异对照**（`render_list_impl.cppm` 的 `a.distanceSq > b.distanceSq` → `<`，还原后复绿）：
overlap 实测变 **(142,115,142)** = **正好等于**反序闭式预测，其余三区照旧逐字节命中，且
`imgqa blend-sort` 的 rc=1 里**只有 `blend-sort-order` 一条红**。
⟹ 判据不只"会红"，而是**两种次序都预测得中**——这是能拿来当验收的判据，不是一台差分噪声计。

**判别力自检**（命令先于对拍跑；备选模型与正确模型的最大字节差，阈值 8 LSB，不足即拒跑）：
反序 = **42**、预乘混合（把非预乘的 SRC_ALPHA 读成 ONE）= **95**。
⟹ 顺带证了一句：混合方程本身也在这个判据看得见的范围内（95 出在 far_only：非预乘给 (0,0.25,0.75)、
预乘给 (0,1,0.75)；near_only 的预乘差是 58），不必另外造一份输入去验"混合是不是非预乘"。

**生成器侧三条对账的变异读数**（各改一处 `build_scene` 的 manifest 落笔，全 RED）：
JSON 写了 `alphaMode="BLEND"` 而 manifest 未申报 → 红 / 写了 `KHR_materials_unlit` 而 manifest
未申报 → 红 / manifest 的 translation 与 node 实际值差 0.5 → 红（第三条措辞点名"node 原点 =
排序代理"，错一位就是判据量错层）。

**已知边界**：这份输入**只**裁合成次序。它不覆盖透明体的光照/阴影/自发光（全 unlit）、MASK discard、
velocity、picking、GPU culling 候选（§4 那几条边界一条没被这份输入碰到）。

**判据本体写在哪**（债-132 结案，2026-10-18）：仪器 = `tools/imgqa/blend_sort.cpp`（复制冻结配置
到 `build/rgl/imgqa_blend_sort_profile` → 跑引擎 → 取四区像素 → 比字节 → 落台账），闭式算式 =
`tools/imgqa/blend_sort_metric.h`(+`.cpp`)（投影 / 合成 / 判别力 / 三条拒跑，**不启引擎、不读文件、
不碰 OpenCV**），契约表 = `tools/imgqa/judgments_table_blend.cpp`（四条，族 = Foundation，阈值同
1 LSB），自检 = `tools/imgqa/selftest_blend_sort.cpp`。三条前置自检**不做成 PASS/FAIL，做成立即抛**
（rc=3）：它们的含义是"这次测量没有意义"，输出一条 PASS 是在撒谎。
⚠ 刀3 当时那份写在仓库外（/tmp）的一次性 Python 脚本自本次起作废，不再是任何读数的出处。

