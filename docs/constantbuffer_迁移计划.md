# scene data 类型化迁移（ByteAddressBuffer → ConstantBuffer / StructuredBuffer）

> **状态**：现行（2026-10-12 立；同日 **C4 结案** = R3 红线窄化放行 + R4 stride 护栏落地，见 §8；
> 同日**开工定案** = C1/C2/C3 全部落定 + 三条新事实（子范围描述符合法 / mapping 必须按 SPV 过滤 /
> 契约矩阵禁用 `float4x4`），见 **§9 —— 开工照 §9 做**）。
> 🔴 **2026-10-13 迁移完成**（分支 `feat/scene-data-typed-migration`）：scene data 十二个区**全部**
> 迁到类型化载体（binding 22–33），`ByteAddressBuffer sceneData` 全区读**已删除并被 R6 永久封死**，
> §2 的成功判据"偏移写不出来了"在 scene data 作用域内**达成**（终态读数见 §9.11）。
> 本文件从"计划"转为**这条线的事实基线 + 每批实测台账**——重做同类迁移前照 §9 读，别重推。
> 本文件 = 这条线的**唯一计划载体 + 事实基线**。
> 触发源 = 簇化轴 ★1-a2-i 期间对"双源为什么必然"的架构诊断（结论见 §1）+ 用户拍板做 spike。
> spike 定案 = `tests/test_descriptor_heap_ubo.cppm`（`TEST_SUITE("gpu")`，git `308a703`）。
> 机制深版仍以各 `.cppm` / `.slang` 文件头注释为权威；本文件不重述 scene data 分区表本身
> （唯一真相源 = `rgl/scene_data.cppm`）。

## 0. 待拍板

| 编号 | 问题 | 结论 |
|---|---|---|
| C1 | 分支拓扑：迁移从 master 切，还是从 MDI tip 切 | ✅ **已结案**：从 MDI tip 切（§6.2 成立）→ 分支 `feat/scene-data-typed-migration` @ `f4ee56c`，锚点 §9.1 |
| C2 | 迁移粒度：每区一 buffer，还是"标量合并一 UBO + 大表各自 SSBO" | ✅ **已结案（§9.2/§9.5）**：**都不选**——物理 buffer 一块不拆，**只拆描述符**（子范围描述符，VUID 依据）；粒度按**语义**划，成本项已被两条机制消掉 |
| C3 | `cull.comp.slang` 遗留偏移是否并入本计划 | ✅ **已结案**：并入，排最后（§9.8「末」批） |
| **C4** | **R3 红线（禁 StructuredBuffer）是否按 §8.4 三条件改精确** | ✅ **已结案（用户 2026-10-12 拍板放行）**：R3 已窄化成"元素类型未走反射契约才 ERROR"，配套护栏 R4 + 设备端 stride 往返已落地 → §8 |

## 1. 为什么现在能做（事实基线，**别再重查一遍**）

诊断结论：项目把跨 CPU/GPU 边界的数据表达成**字节**、把跨边界的事实表达成**注释**。
上游只有一个决策——scene data 是**单个异构巨型 buffer**（单描述符 + 单次 staging memcpy），
而 `StructuredBuffer<T>` 要求全缓冲单一元素类型，异构 ⟹ 用不了 ⟹ 只能 `ByteAddressBuffer`
⟹ 类型在边界处被擦除 ⟹ 每个消费点手工再物化一次布局。

**"ByteAddressBuffer 是 descriptor heap 专用"是历史传说，已实测证伪**（本机 AMD 8060S /
RADV STRIX_HALO / Mesa 26.2.3）：

- 三臂全通：`ConstantBuffer<T>` + `eUniformBuffer` 7/7 槽逐字节正确、`StructuredBuffer<T>`
  4/4、`ByteAddressBuffer` 对照 2/2。
- Slang 产物：`Uniform` storage class + `Block`（非 `UniformConstant` / `BufferBlock`）；
  scalar `Offset` 与 C++ `offsetof` **逐字段相同**（`float3` 占 12B 不 pad）。
- UBO 与 SSBO 描述符**同宽 16B、同对齐 4B** ⟹ 引擎统一 32B 步长不用改。
- descriptor 写入路径与现有 `eStorageBuffer` **完全同构**（只差 `desc.type` 一个字段）。

🔴 **顺带定案一条机制**（比主结论更值钱，且推翻了一条长期口头假设）：**binding 号 ≠ heap 槽号**。
真实机制 = `vkCreateShadersEXT` 期由 `VkDescriptorSetAndBindingMappingEXT::
sourceData.constantOffset.heapOffset`（字节）**逐 binding 烘焙**（`rgl/shader.cppm`）。数值相等
只是调用方按槽喂出来的巧合。**任何"按 binding 号算槽"的新代码都是错的。**

spike 的三条判据（主判据 / 阳性对照 / 反向对照）+ 主 agent 亲做的地址 `+4` 变异（红 7 条断言）
见 `tests/test_descriptor_heap_ubo.cppm` 头注释。

### 1.1 一条**带失效条件**的负知识（别读宽）

**作用域**：`maxUniformBufferRange` 本机 = 4294967295，但 Vulkan 1.0 规定下限 **16384**、
NVIDIA 常见 **65536** ⟹ 在本机把 787KB 的 scene data 整体做成一个 UBO 会通，在别家会失败。
**仍然要做**：照做小标量区 → UBO 的迁移（那些区是百字节量级，远低于任何下限）。
**什么实验推翻它**：在 NVIDIA / Intel 上实测 `maxUniformBufferRange` ≥ 目标区大小。
**结论不是"别用 UBO"，是"UBO 尺寸必须运行期查询 + 不够则回落"**（§3.1）。

### 1.2 spike **没有**回答的三件事（别把本计划读成"已验完可批量改"）

1. **逐位未验**：spike 不碰渲染路径。任何迁移批次的验收都是 `just regress` 逐位，不是"看起来一样"。
2. **per-draw 换 UBO 未验**：机制上通（换绑定 = 重写 heap 槽、不动 shader object），但换绑定的
   **时序 / barrier 要求**没测。这条是 MDI 命令数轴的需求，归 MDI 线，不归本计划。
3. **拆 buffer 的 CPU 代价未量**：每帧描述符写入数与 staging 拷贝次数都会随拆分上涨。→ 批 0。

## 2. 目标形态（三类区 → 三种载体）

| 区的性质 | 载体 | 偏移 | 反射 |
|---|---|---|---|
| 每帧标量参数（头 128B、AO、曝光、SSR、湿面、TAA、阴影辅助参数…） | `ConstantBuffer<T>`（UBO） | **无** | struct 进 schema，`sizeof` + 逐字段 `offsetof` assert |
| 大 per-object 表（对象矩阵、draw 表、簇区间表） | `StructuredBuffer<T>`（SSBO） | **无基址**，只有下标 | 同上，`ArrayStride` 进产物 |
| 确需按字节切的大池（簇顶点池等，若仍适用） | `ByteAddressBuffer` | 有 | 🔴 必须**申报**（§5.2 棘轮），逐个写明为什么不能类型化 |

**目标 idiom**：

```slang
// 契约：struct 声明在反射契约文件里，schema 登记 → 生成 C++ struct + static_assert
StructuredBuffer<MeshletCullItem> cullItems;   // 无基址字面量，元素布局由反射钉死
StructuredBuffer<uint>            clusterVisible;
```

对照现状（要消失的形态）：`sceneData.Load<ObjectDraw>(524944u + objectIndex * 48u)`。

> ✅ **本计划的 §2 目标形态已放行（2026-10-12，C4 结案）**。曾经挡着它的 `shader_dep_check`
> **R3**（`tools/check_shader_deps.py`）从「任何 .slang 出现 StructuredBuffer = ERROR，全库零
> 使用，**无豁免清单**」**窄化**为「`StructuredBuffer<T>` 的元素类型 T **不在反射生成契约类型
> 集合内**才 ERROR」——保留牙（无契约的裸 struct 仍然红），去掉误伤（契约类型放行）。
> 配套两道护栏，**缺任一道都不许再写 `StructuredBuffer<T>`**：
> **R4** = `tools/check_spirv_stride.py`（每次 `just build` 现场编探针，断言 SPIR-V
> `ArrayStride` == 反射生成物 `sizeof`，失效必出声）；**设备端往返** = `just test-gpu
> -tc="*stride*"`（stride 8 与 44 的逐槽位读回 + 阳性对照）。
> 窄化的全部依据（含推翻旧理由的实测）与放行前的经过 → **§8**。

> 🔴 **本计划的成功判据不是"偏移被生成对了"，是"偏移写不出来了"**。凡是最后落成"生成一份
> 偏移常量给 shader 用"的批次，都说明那块没真正迁完，只是把双源搬了个家。

> ✅ **§2 成功判据达成（批 N+1，2026-10-13 本机 AMD 8060S / RADV STRIX_HALO）**——而且是
> **机器可验的达成**，不是"看起来没了"：
> ① `ByteAddressBuffer sceneData` 声明已从 `rgl/shaders/scene.slang` 删除，剥注释后
> `rgl/shaders/**.slang` 里 `sceneData` **零命中**（`git grep -n sceneData -- '*.slang'` 的
> 全部剩余命中都在注释里，是历史散文）；
> ② 永久护栏 **R6**（`tools/check_shader_deps.py`，挂号 → `docs/风格检查规则.md` §6.3）把
> "写回来"变成 **ERROR + `--strict` 退 1**，三条变异实测有牙（加回声明+一条 `Load4` /
> 扫描面置空 / 扫描面换错目录，逐条读数在挂号处）；
> ③ R5 棘轮 **130 → 49**，且**剩余 49 条逐条核实全部不属于 scene data**（归属表 → §9.10）。
> ⚠ **达成范围要说清**：判据说的是 **scene data 这一块**。仓库里仍有别的 `ByteAddressBuffer`
> 带裸偏移（meshlet 产物表 / gpu_cull 私有契约 / IZB 链表 / 顶点索引池），它们各有自己的
> 契约归属，**不在本计划射程**（逐条见 §9.10）；"偏移写不出来了"这句话的作用域 = scene data，
> 读宽了会误报"没做完"。

## 3. 硬约束与红线

### 3.1 UBO 尺寸必须运行期查询

复用 `checkMeshletDeviceFit` / `warnIfMeshletDeviceUnfit` 的形态（`rgl/main_meshlets.cppm`）：
查 `maxUniformBufferRange`，不够则**不启用该载体**并 warn 出声，而不是静默画错。口径 §1.1。

### 3.2 逐位是判据，不是愿望

迁移**不改任何数值语义**——只改访问机制。所以每一批的验收都是：

- 门关档截图 sha **逐位 =** 本批分支点的锚点；
- 门开档（若该批带门）逐位 = 该批自己的锚点。

🔴 **分支点锚点必须在开批之前记下**，事后补锚点 = 把"和什么比"这件事交给记忆。
现行锚点：门关 `54e406655f16faf0674b382818bd589f875c8ee0705cdec1c109d5ff1a72df4f`。
⚠ 2026-10-12 起**门关 ≠ 出厂默认**（★18-C 翻了 `meshlet` 默认）：到达门关要显式
`--set meshlet.enable=false --set meshlet.shadow_lod=-1`，出厂默认是另一个锚 → 总账红线声明块。

### 3.3 反射接入门槛

新载体必须有 schema（`constants` + struct 布局）。🔴 **反射不校验 `constants`**
（`tools/generator.cppm`：「反射端无对应数据，直接由 schema 声明生成」）⟹ 需要机器校验的
数值契约走 `tools/checks/` 规则，先例 = `meshlet_spec_checks`（三段判据：数值对账 /
**关系对账** / 死常量）。

### 3.4 静默失效清单（本类迁移特有的三种假绿）

- **漏写描述符完全静默**：spike 反向对照实测——UBO 槽留空（null descriptor）读到零，
  **不挂死、不 device lost、VVL 不报错**。⟹ 迁移后"画面没变"不能证明新描述符被消费了。
  每批必须带**阳性对照**（强制读错成员/错下标，画面必须变）。
- **读到界内错地址**：VVL 默认档不插桩 shader 运行期地址，逐位回归只在错值恰好改变被比对
  帧的像素时才看得见。唯一能看见它的工具是 **GPU-AV**（环境变量级，项目从未启用）→ §5.3。
- **死 offset 常量**：区迁走后忘了删 `kSceneData*Offset`，它仍"被 C++ 引用"从而看似在用
  ⟹ §5.1。

## 4. 分批（每批一个可回退单元）

> 🔴 **本表已被 §9.8 取代**（批 0 的"拆 buffer 代价"经 §9.2 证明为 0，不再需要先量再定；
> 实际执行序与每批状态以 §9.8 为准）。保留本表只为留下"当初为什么这么排"的推理。

| 批 | 内容 | 判据 | 为什么这个顺序 |
|---|---|---|---|
| **0** | 代价测量：拆 buffer 后每帧描述符写入数 / staging 拷贝次数的增量 | 数字进本文件，**不达标就停整个计划** | 🔴 先量再定粒度（C2），否则形态是拍脑袋定的 |
| **1** | **一个**最小真实区（建议湿面 32B 或 SSR 96B）做**双路对拍**：老 ByteAddressBuffer 路 vs 新 UBO 路，门切换 | 两路逐位相同 + 阳性对照必须变 | 单区打通全流程（schema→反射→heap 写→barrier→逐位），失败代价最小 |
| 2..N | 按区推进，**从消费者最少的区开始** | 每批独立逐位锚点 | 消费者数 = 改动面 = 风险 |
| N+1 | 删 `scene.slang` 失效 read 函数与 `scene_data.cppm` 失效 offset 常量；`meshlet.mesh.slang` 的 `44+8/12/16/20` 字面量 | 删除后 build + 逐位不变 | 删除才是真收益；不删 = 双源只是被藏起来 |
| 末 | `cull.comp.slang` 遗留偏移（C3） | 同上 | 纯遗留，不阻塞新功能 |

## 5. 护栏（**先建护栏再迁移**，顺序反了护栏永远补不上）

1. **死 offset 常量检测**：`kSceneData*Offset` 若其对应区已无 shader 侧消费者 → 警告。
   否则迁移留下的尸体会被读成"这个区还在用"。
2. 🔴 **裸字节偏移棘轮**：新增 `Load*(... + <数字>)` 形态 = **error**；存量进 baseline
   **只准减不准增**（机制先例 = `tools/checks/tool_baseline.py`）。这条是唯一能阻止
   ByteAddressBuffer 再扩散的护栏——**没有它，本计划做完后新代码照样写偏移**。
3. ~~**GPU-AV 冒烟臂**~~ → 🔴 **已否决**（2026-10-12 实测，依据见 §9.9）。替代护栏 =
   **表尾毒化哨兵元素**（§9.9 末），它覆盖 §3.4 第二条的"读到界外"那一半。
4. **设备能力闸门**：任何编码了设备上限的着色器常量，必须有对应的运行期查询。
   先例 = `checkMeshletDeviceFit`。

## 6. 与簇化轴（MDI）的边界与排序

### 6.1 文件归属（两个会话并行时的冲突面）

| 归属 | 文件 |
|---|---|
| 迁移会话 | `scene.slang` 的 read 函数、`scene_data.cppm`、各 shader 的 scene 读调用点 |
| MDI 会话 | `meshlet.*`、`render_list*`、`main_meshlets*`、`vkrenderer_passrec*`、`gpu_cull*`、新增 buffer |
| 🔴 **重叠** | `scene.slang` / `scene_data.cppm`（MDI 侧只剩 `ObjectClusterRange`，**已落地**） |

结论：重叠面**已经收窄到接近零**，前提是 MDI 剩余批次**不再往 scene data 加新区**。
若 β-2（阴影段）需要新 scene data 区，**必须先跟迁移会话打招呼**，否则一定冲突。

### 6.2 🔴 分支拓扑：从 MDI tip 切，不要从 master 切

迁移的验收判据是**逐位 = 分支点锚点**。所以：

- 从 **master** 切 ⟹ 锚点是 master 的像素，而 MDI tip 的像素**已经不同**（β-1 原位取代改过）
  ⟹ 合并后"迁移是 no-op"这个断言**无法在合并结果上复核**，只能重新导一份基线，
  而那份基线里混着 MDI 的渲染改动 ⟹ 以后再也说不清某个像素是谁改的。
- 从 **MDI tip** 切 ⟹ 锚点有效、迁移的 no-op 断言精确成立、MDI 侧不需要复验任何东西，
  且迁移分支天然是 MDI 的后代，合并是快进级别。

**所以：MDI 先落，迁移从落完的 master 切。** 若现在就想并行开工，正确做法是
**从 MDI tip 切迁移分支**（不是 master），迁移会话自己带一份有效锚点，MDI 落完后 rebase——
rebase 是平凡的，因为它本来就是后代。

### 6.3 ★1-a2-i 试过用新 idiom 写，**已因 R3 撤销**（见 §8）

原判断：MDI 的 GPU 侧（剔除项表、可见标志 buffer）是新代码、无逐位锚点，用 `StructuredBuffer<T>`
写等于零风险把新机制在渲染路径跑通一遍，顺便给本计划立好实际样例。

**实测结果**：三张表全部类型化后，`ArrayStride` = 8/44/4、`cull=on` 与 `cull=off` 逐位相同、
阳性对照（强制标志全 0）几何整块消失——**功能与布局都对**。但 `shader_dep_check` R3 报 4 条
ERROR（红线明令"无豁免清单"）⟹ **已回退成 `ByteAddressBuffer` + `Load<T>`**，功能不受影响。

> 保留的一点：`Load<T>` 形态下步长与字段位置**仍由编译器从契约 struct 算**，所以 ★1-a2 至今
> 没有手抄的字段偏移。R3 挡的是 stride 布局的静默偏离，与"不写字面量"是两件事——别把回退读成
> 回到了 `cull.comp.slang` 那种手抄偏移。

## 7. 明确不做

- **不追求 push constant 替代**：`maxPushDataSize = 256` 够用，但 DGC 的 `@8 PUSH_DATA`
  有已知问题（`docs/mdi_plan.md` 挂着），在 indirect 路线定案前引入 push constant 是叠风险。
- **不动 descriptor heap 机制本身**（spike 已证它不是障碍）。
- **不在本计划里改 scene data 的任何语义或数值**——那是功能改动，会污染逐位判据。
- **不追求"一个区都不剩"**：§2 第三类允许保留 `ByteAddressBuffer`，但必须申报理由。

## 8. ✅ 已结案：R3 红线按 §8.4 窄化放行（**用户 2026-10-12 拍板**）

> **终态一句话**：R3 从「出现 `StructuredBuffer` 即 ERROR」窄化为「**元素类型未走反射契约**
> 才 ERROR」，配套 **R4 stride 对账护栏**（构建期）+ **设备端 stride 往返用例**（`just
> test-gpu`）同日落地，§2 目标形态自此可在 `rgl/` 树内实施。
> 🔴 **但"放行"不等于"立刻迁移"**：现存 `meshlet_cull.comp.slang` / `meshlet.mesh.slang`
> 挂着逐位锚点，改回类型化载体是**单独一批**（要跑逐位对拍），与本结案无关。

### 8.1 红线本体（历史形态，2026-10-01 立 → 2026-10-12 窄化）

`tools/check_shader_deps.py`（构建期 `shader_dep_check`，`cmake/RglChecks.cmake` 登记）：

> **R3 任何 .slang 出现 StructuredBuffer（含 RW/Append/Consume）= 红线（全库零使用，
> 无豁免清单；git:调试历史「灰屏」(2026-06-24) stride 静默错位）**

只警告不阻断（醒目 ERROR 行、`--strict` 拿非零码）。规则自陈的理由是"stride ≥16B 对齐与
C++ sizeof 静默偏离"——**这条理由的普适性已被 §8.5 实测推翻**。

### 8.2 当时发生的事，按时间顺序（**含一个我该早点发现的疏漏**）

1. spike（`tests/test_descriptor_heap_ubo.cppm`）用 `StructuredBuffer<Mvp>` 验通了 heap 内的
   SSBO/UBO。**但 R3 只扫 `rgl/` 树、不扫 `tests/shaders/`** ⟹ **spike 是绕着红线跑的，
   不是红线认可了它**。我在写 §2 目标形态时没有先查这条规则，这是疏漏。
2. ★1-a2-i 的 GPU 侧按 §2 写成类型化载体（3 张表），实测：`ArrayStride` = 8/44/4、
   `cull=on` 逐位 = `cull=off` = 取代档锚点 `0233baacea7b2654…`、阳性对照 sha `734124f3…`
   （两次独立跑同值）。**功能与布局都正确。**
3. 代价 = R3 报 4 条 ERROR。红线写明"无豁免清单"，⟹ **已回退** `ByteAddressBuffer` + `Load<T>`，
   回退后逐位仍 = `0233baacea7b2654…`、R3 归零。

### 8.3 这条红线**盖住了什么**、**没盖住什么**（别照字面读宽）

- 它怕的失效形态是真的：元素 stride 在 shader 侧与 C++ 侧不一致 → 静默错位 → 灰屏。
- 但它**一刀切**的形态下，"元素类型来自反射契约"这一类**恰好是最不可能静默偏离的**：C++
  `sizeof` 与 SPIR-V `ArrayStride` 同源于一份声明，生成物还带 `static_assert(sizeof == N)`。
  也就是说 R3 现在**把它的解药和病毒一起禁了**。
- 反过来，R3 **不**保证现存 `ByteAddressBuffer` 路径是对的——那条路上偏移是手抄的，
  真出错时同样静默。今天 `MeshletClusterHeader` 那批字面量就是靠人肉发现才删掉的。

### 8.4 撤销条件 → **三条全部落地（2026-10-12）**

| # | 条件 | 落地形态 | 有牙证明 |
|---|---|---|---|
| 1 | **R3 改精确**：从"出现 StructuredBuffer 即 ERROR"改成"**元素类型未走反射契约**才 ERROR" | `tools/check_shader_deps.py` R3；契约集合机械取自 `rgl/generated/*_types_generated.cppm`（**不是手抄豁免清单**——名单随 schema 自动长） | 临时塞一个 `StructuredBuffer<BareDude>`（无契约裸 struct）→ 2 条 ERROR、`--strict` 退 1；同文件里 `StructuredBuffer<MeshletClusterHeader>` 与 `StructuredBuffer<uint>` 放行 |
| 2 | **stride 实测护栏**（构建期） | **R4** = `tools/check_spirv_stride.py`（挂在同一个 `shader_dep_check` 目标，每次 `just build` 跑）：按定义模块合成探针、用**构建期同一套 `SLANG_FLAGS`** 现场编 SPV、解析二进制断言 `ArrayStride == sizeof` | 见 §8.6 变异读数 |
| 2′ | **stride 设备端往返** | `tests/test_descriptor_heap_ubo.cppm` 的 `dheap 4/5`（`just test-gpu -tc="*stride*"`）：stride **8** 与 **44**，写已知模式到**元素 1**（元素 0 在 stride 错位时恰好也对，只读它等于没测 stride），期望值走闭式解 | shader 镜像 `float3 center`→`float4`（stride 44→48）⟹ 8 个 stride-44 槽位全红、3 个 stride-8 槽位不动；阳性对照改回同下标 ⟹ 恰好被改的 2 个槽位红 |
| 3 | **逐位护栏** | §3.2 不变：迁移批次逐条 `just regress`，阳性对照必须真变 | 既有纪律，本批未动渲染路径 |

🔴 **R4 是 R3 能窄化的前提，不是附属品**：R3 管"只准用有契约的类型"，R4 管"有契约的类型
stride 真的对"，两条合起来才把灰屏那段静默空间关掉。**拆掉 R4 而不恢复 R3 全禁 = 放行裸奔。**

### 8.5 🔴 红线理由的**实测推翻**（2026-10-12，本机 slangc + 项目原 flags）

旧理由写的是「`StructuredBuffer<T>` 强制元素 stride ≥16 字节对齐」，且旧调试历史另有一句
「`scalarBlockLayout` 不影响其 stride」。**两句都被实测推翻**：同一个 `struct Vertex { float3
pos; float3 color; }`（C++ `sizeof` = 24），只改一个 flag：

| flags | SPIR-V 结构体名 | `color@Offset` | **ArrayStride** |
|---|---|---|---|
| 项目现行（带 `-fvk-use-scalar-layout`） | `Vertex_natural` | 12 | **24** ✓ = C++ `sizeof` |
| 不带 scalar layout | `Vertex_std430` | 16 | **32** ✗ |

旁证（同一构建产物，非推测）：`build/rgl/generated_shaders/meshlet_cull.comp.spv` 里
`MeshletClusterHeader`（含 `float3 center`，44B）的 `ArrayStride` = **44**，不是 48。

**三条结论**：

1. **灰屏是真的、机制也记对了**（24 vs 32），**但根因是 std430 把 `float3` 补到 16 字节**，
   不是"StructuredBuffer 强制 16 字节 stride 对齐"这条固有属性。旧记录把**一种布局口径的
   行为**写成了**载体的固有属性**——这是本类红线最容易犯的过度泛化。
2. **「`scalarBlockLayout` 不影响其 stride」经验上是错的**：scalar layout 把 `ArrayStride`
   从 32 改成 24、`color` 从 16 改成 12。也就是说**项目全量带 `-fvk-use-scalar-layout`
   （`cmake/RglSlang.cmake` 的 `SLANG_FLAGS` 唯一声明点）这件事本身，就是那条红线的前提失效**。
3. **当年选的"正确修法"（`ByteAddressBuffer` + 手写 `.Load3(ti * 24)`）正是本项目今天双源
   问题的制造者**——拿一个**编译器能抓**的坑（stride 不匹配 → 现在由 R4 直接 ERROR），
   换成了一个**只有截图能抓**的坑（手写字节偏移写错，静默画错）。这条讽刺是本次结案最值钱
   的产出：**一条被过度泛化的红线，其"解法"的代价不会出现在红线的账上**，只会出现在半年后
   那份"为什么这块偏移有两份"的排查记录里。

**所以**：2026-10-01 全禁**当时是对的**（那会儿不在全项目 scalar layout 下、也没有 R4）；
2026-10-12 全禁**现在是错的**（全项目带 scalar layout、元素类型出自反射契约带
`static_assert(sizeof == N)`、且 R4 每次构建对账）。

### 8.6 变异读数（护栏有牙的原始证据，2026-10-12 本机）

```
# 变异 A：把 MeshletClusterHeader 的 schema 声明值 44 改成 48
[stride-check-ERROR] rgl/shaders/meshlet_cull.slang: `StructuredBuffer<MeshletClusterHeader>`
  的 SPIR-V ArrayStride = 44 ≠ 反射生成物 sizeof(MeshletClusterHeader) = 48   → exit 1

# 变异 B：去掉 -fvk-use-scalar-layout（= 复现当年灰屏的机制本身）
[stride-check-ERROR] … MeshletClusterHeader  ArrayStride = 64 ≠ sizeof = 44（类型名 …_std430）
[stride-check-ERROR] … MeshletCullPush       ArrayStride = 112 ≠ sizeof = 100
[stride-check-ERROR] … ModelVertex           ArrayStride = 64 ≠ sizeof = 48
[stride-check-ERROR] … SceneLight            ArrayStride = 64 ≠ sizeof = 48   → exit 1

# 失效出声：slangc 定位不到时不静默返回 0
[stride-check-ERROR] --slangc 指定的路径不存在: /nope/slangc——无法现场编探针，
  本护栏本次**未检查 9 个契约类型**
[stride-check] 汇总: 1 问题（ERROR 1） / **无读数 = 本次未做任何对账**
```

### 8.7 为什么这条**必须**用户拍板（结案陈词）

"全库零使用、无豁免清单"是一条**意图声明**，不是一条技术结论。工程侧能证明"带反射契约的
StructuredBuffer 在本机 stride 正确"，但**推翻一条用户定下的红线不是工程权限**。
2026-10-12 用户拍板放行，§8.4 三条同日落地，本计划 §2 目标形态自此解禁。

## 9. ✅ 开工定案（2026-10-12 晚，分支 `feat/scene-data-typed-migration` @ 分支点 `f4ee56c`）

> 本节 = C1/C2/C3 的**结论** + 开工前必须钉死的事实。**迁移照本节做，别再重新讨论。**
> MDI 阶段性结束、无并行会话（用户拍板），本分支从 MDI tip（= 当时的 master 前沿）切，
> C1 按 §6.2 建议成立。

### 9.1 分支点锚点（开批前记下，§3.2 红线）

| 档 | 命令 | sha256 |
|---|---|---|
| **门关**（护栏态） | `just shot --set meshlet.enable=false --set meshlet.shadow_lod=-1` | `54e406655f16faf0674b382818bd589f875c8ee0705cdec1c109d5ff1a72df4f` |
| **出厂默认**（meshlet 开） | `just shot`（不带 `--set`） | `95edc8ccd7a4f2a5d72094007a68aa5b0fa41aed7d490397b30730fa7dc7908c` |

门关档实测**仍 = 文档旧锚** `54e40665…`（★18-A/B1/B2/C 只动 meshlet 路径）。
**每一批两档都要量**：门关证明"没碰旧语义"，默认档证明"没碰 meshlet 叠加路径"。

### 9.2 C2 定案：**一个物理 buffer + N 个子范围描述符**（拆描述符，不拆 buffer）

这是本计划最省的一笔。三条依据：

1. **VUID-VkResourceDescriptorInfoEXT-type-12350 / -12351**（本机 spec 克隆
   `chapters/descriptorheaps.adoc:412,417`）：UBO / SSBO 描述符的 `pAddressRange->address`
   只需分别是 `minUniformBufferOffsetAlignment` / `minStorageBufferOffsetAlignment` 的整数倍
   ⟹ **允许指向 buffer 内部的非零偏移**（spike 只测了整块 buffer，这条是本次补的）。
2. **VUID-…-11461 / -11462**：只要求那块 buffer 建时带 `UNIFORM_BUFFER_BIT` /
   `STORAGE_BUFFER_BIT`（同一块两个 usage 合法）；**VUID-…-11433**：size 不得为 0。
3. 描述符是**启动期写一次**的静态地址范围（现状 `SceneDataService::ensure` 就只写一次）。

⟹ **每帧 CPU 代价增量 = 0**：staging ring、逐字段 memcpy、`vkCmdCopyBuffer` 一条不多不少。
§1.2 第 3 条"拆 buffer 的 CPU 代价未量"与 §4 批 0 的"不达标就停"因此**不构成风险**——
要拆的只是**描述符**（+N 个 heap 槽，每槽 32B，运行时段容量 1024 用不完）。

**布局红线**：所有子区基址静态 **256B 对齐**（覆盖桌面端 `minUniformBufferOffsetAlignment`
的常见上限），buffer 基址用 `VmaAllocationCreateInfo::allocationAlignment = 256` 钉住；
并加**运行期闸门**（§3.1 形态，先例 `checkMeshletDeviceFit`）：查
`minUniformBufferOffsetAlignment` / `minStorageBufferOffsetAlignment` / `maxUniformBufferRange`，
不满足则**响亮失败**（不是静默画错）。

### 9.3 定案：mapping 按 **SPV 实际声明的 binding** 过滤 ✅ **已落地**（批 M，`1b48f27`）

**动机原样保留**：`RenderPipeline` 原先无条件烘 mapping（`hasSceneData` 一个布尔烘一条
binding 3）。迁移要加 12 条 binding，若沿用旧形态就得在 C++ 侧逐条目登记"谁用哪几条"
——**那正是本项目最反感的抄名单反模式**（名单随 schema 漂）。
解法：构造期解析 mesh + frag 两份 SPV 的 `OpVariable` 装饰集，**只为真声明的 binding 发
mapping**；解析失败退回全烘 + warn 点名条目（全烘恒正确 = fail-safe 方向）。
⟹ 新增 binding **不需要**在 C++ 侧登记消费者，`PipelineBindings` 无脑带上全部载体槽即可。

**🔴 但本批实测推翻了我原先写在这里的预估，照实测改口径（2026-10-12 本机）**：

| 读数 | 改前 | 改后 |
|---|---|---|
| mappings（出厂默认档） | 6560 | **3722**（−43%） |
| mappings（门关档） | — | 2265 |
| createTotal | 2.93s | **2.95s**（噪声内，**没降**） |
| 退回全烘的条目 | — | **0 / 479**（958 份 SPV 全部解析成功） |

原写"mapping +73% ⟹ 启动接近翻倍"**不成立**：本机 `vkCreateShadersEXT` 由**着色器编译**
主导，mapping 条数的边际成本近零。**所以批 M 的收益不是启动提速，而是"消灭手工消费者
清单"这条结构性质**——别拿提速当它的验收判据。（教训形态：**成本预估也是假设**，
"按 X 面外推"在没测过边际成本前不是结论。）

**🔴 顺带钉死一条对本计划风险面影响很大的事实**（变异 A 实测，非推理）：
"SPV 声明了 binding 却没烘 mapping" **不是静默读零**，而是 `vkCreateShadersEXT` 期被 VVL
**硬拦**并报 `VUID-VkShaderCreateInfoEXT-flags-11292`（"has no mapping for [Set 0, Binding N,
variable …]"），引擎启动即死。
⟹ 迁移期"加了 binding 忘了接线"的失效形态是**响的**，与 §3.4 第一条（**反向**：漏写
**描述符**完全静默）正好互补。两侧别记混：**描述符漏写 = 静默读零；mapping 漏写 = 启动炸**。

### 9.4 定案：契约里的矩阵**继续**用 `ObjectMatrix`（4 个具名 float4），禁止 `float4x4` 字段

反汇编实测（2026-10-12 本机 slangc + 项目原 flags，`-fvk-use-scalar-layout`）：

```
OpMemberDecorate %H_natural 0 Offset 0
OpMemberDecorate %H_natural 0 RowMajor        ← Slang 对 float4x4 成员发 RowMajor
OpMemberDecorate %H_natural 0 MatrixStride 16
…  m[0]  →  OpLoad %mat4v4float + OpCompositeExtract %v4float … 0
```

SPIR-V 矩阵**逻辑上是列数组**，`RowMajor` 只改内存布局 ⟹ `m[0]`（逻辑列 0）在 RowMajor
内存下是**跨行 gather**，**不等于**内存第 0 个连续 16B。而 `ObjectMatrix.c0` = 内存第 0 个
连续 16B = glm 的列 0。两者差一个转置——正是 `scene.slang:73-79` 那条"带旋转的建筑直接
炸形"的坑的同一机制的另一面。
⟹ 契约 struct 里凡矩阵一律 `ObjectMatrix`（4×float4，反射可查、C++ 侧 `sizeof` 64），
列组合走既有 `mulM` / `clipPos` / `worldPos` 惯用法。**不引入 `transpose()` 兜圈子**。

推论：`ObjectMatrix` 要能**作为字段嵌进** UBO 契约 struct，而反射生成器当前不支持嵌套
struct（`slang_reflect.cppm::typeToCppType` 对 `Kind::Struct` 落到 `default: return ""`
= unmappable 致命）。⟹ **批 R 先给反射工具补嵌套 struct 支持**（递归类型名 + 依赖先出 +
schema 交叉校验），这是"反射工具为主导"这条目标本体的活，不是绕路。

### 9.5 区 → 载体定案（C2 的粒度结论：**按语义划，不按成本划**）

成本已被 §9.2（子范围描述符）+ §9.3（mapping 过滤）两条消掉 ⟹ 粒度选择**不再有成本项**，
按语义边界划即可。binding 3（`sceneData` ByteAddressBuffer）在迁移期保留，各区迁完即从
`scene.slang` 删对应 read，**全部迁完后 binding 3 与 `sceneData` 声明一起删**。

| 载体 | binding | 类型 | 内容 | 备注 |
|---|---|---|---|---|
| `SceneHeader` | 22 | UBO | light(48) + envParams + exposure + aoStrength + skyInvVP | 消费者最多，**最后迁** |
| 对象矩阵表 | 23 | `StructuredBuffer<ObjectMatrix>` | ×4096 | |
| `LightArray` | 24 | UBO | count + 8×`GpuLight` | 528B，UBO 装得下 |
| 对象 MVP 表 | 25 | `StructuredBuffer<ObjectMatrix>` | ×4096 | |
| 材质/几何表 | 26 | `StructuredBuffer<ObjectDraw>` | ×4096 | 已是契约类型 |
| `IrradianceSH` | 27 | UBO | 9×vec4 | |
| 阴影 VP 区 | 28 | UBO | `ObjectMatrix[4]`（嵌套 + 数组） | 走 UBO 不走 SSBO：它是"4 个固定槽的参数" |
| `ShadowParams` | 29 | UBO | splits/texel/params/atlas/izb/pcss/evssm/evssmConst | 合并原 7 个小 vec4 子区 |
| `TaaParams` | 30 | UBO | prevViewProj / motion / params / prevShift / inputKernel / clipControl | |
| `SsrParams` | 31 | UBO | viewProj / params / gate | |
| `WetParams` | 32 | UBO | params / spatial | **批 1 试点**（1 个消费者、无矩阵） |
| 簇区间表 | 33 | `StructuredBuffer<ObjectClusterRange>` | ×4096 | 已是契约类型 |

目标 idiom 达成形态（对照 §2 的"要消失的形态"）：`objectDraws[i].baseColor`、
`frame.taa.params.x`——**shader 里一个字节数都不写，也没有基址**。

### 9.6 验收口径的**一处修订**（不是放松，是换更强的算子）

§4 批 1 原写"双路对拍：老路 vs 新路，门切换"。实施结论：**逐位对分支点锚**（§9.1）比
"同一 binary 内门切换"**更强**——它比的是同一帧同一构建的像素，而门切换要引入 SPV 变体
× 区数的组合税，且门本身是新代码。所以每批的判据固定为三条 AND：

1. 门关 + 出厂默认**两档**截图 sha 逐位 = §9.1 锚点；
2. **阳性对照**（把新载体读错成员/错下标，画面必须变）——§3.4 第一条"漏写描述符完全静默"
   只有这条能排，**缺它整批不算过**；
3. `just test` / `just test-gpu` / `just checks` 全绿。

批 1 额外做一次真正的**双路**（新旧载体并存、逐位相同），用来把"schema→反射→heap 写→
barrier→逐位"全链路第一次打通；之后各批按上面三条。

### 9.7 定案：布局**原地重排到 256B 对齐**，迁移期**不复制数据**（批 L）

§9.2 说"拆描述符不拆 buffer"，但子范围描述符的**地址对齐**是硬约束
（`minUniformBufferOffsetAlignment` / `minStorageBufferOffsetAlignment`，VUID-…-12350/12351），
而现状各区基址只保证 **16B** 对齐（`128` / `262800` / `524944` / `722400` 全都过不了 32/256）。
⟹ 必须先重排布局，否则载体根本挂不上去。

**两条被否掉的路**（别再想第三次）：

- ❌ **迁移期把数据写两份**（旧字节区 + 新载体区）：per-object 表合计 ~768KB，
  等于把 buffer 与每帧 staging memcpy 翻倍，而"翻倍"这件事会一直挂着直到 N+1 批才拆。
- ❌ **只给新载体追加一段 256 对齐的尾区**：小参数区可以，大表不行（表要么搬、要么留旧地址，
  搬了旧字面量就得改——绕回原地重排）。

✅ **定案 = 原地重排**：`scene_data.cppm` 的区基址常量全部改成**按 256B 向上取整**派生
（`kSceneRegionAlign = 256`，buffer 分配 `allocationAlignment = 256` 钉住基址），
`scene.slang` 侧尚未迁移区的字节字面量随之同步（**两侧 static_assert 本来就钉着，
改错编译不过**）。数值语义一 bit 不动 ⟹ **两档锚点逐位不变就是本批的全部判据**。

之后每批迁移 = ①契约 struct 进 schema + `just reflect` ②`SceneDataService` 给该区写一条
子范围描述符（`{base + 区基址, 区大小}`，UBO 区用 `eUniformBuffer`、SSBO 区用 `eStorageBuffer`）
③`scene.slang` 的 read 函数从"字节 Load"改成"类型化访问"。**零数据复制、零每帧代价变化。**

🔴 `PipelineBindings` 带上**全部**载体槽、由批 M 的过滤自动裁掉未声明的——**不在 C++ 侧
登记消费者**。代价：`RenderPipeline` 的 `mappings[]` 容量要从 24 抬到 ≥48（过滤前计数）。

### 9.8 分批执行序（每批一个可回退单元，子 agent 推进 + 主 agent 校验）

| 批 | 内容 | 状态 |
|---|---|---|
| **R** | 反射工具补**嵌套 struct** 支持（§9.4 推论）+ 有牙判据 | ✅ **已落地**（2026-10-12 本机 AMD 8060S / RADV STRIX_HALO）：`typeToCppType` 加 `Kind::Struct`（struct 名直出；拿不到名字 = 空串走既有 unmappable 失败路径，**不兜底**）；`vertexAttrCppType` 认嵌套契约名——合法判据**定死** = 名字出现在本次 schema 的 `vertexLayouts` 名单（数组形态 "GpuLight[8]" 同规则），拼错名仍空串；`generateCppmFromSlang` 加**依赖先出**校验（违规 `[FAIL]` 点名"把 X 排在引用它的 Y 之前（发射顺序 = schema vertexLayouts 书写顺序）"）；`validateSlangVertexLayout` 逐字段比对天然覆盖嵌套（变异 A 实测证明未跳过）。`ObjectMatrix` 登记进 scene_forward_schema（stride 64，c0..c3 @0/16/32/48）→ 进生成物 → **R3 契约面 10→11**（`StructuredBuffer<ObjectMatrix>` 自动放行，后续 MVP/对象矩阵表批受益）、R4 对账 11/11 零 ERROR。有牙判据三条实测（临时 `RtmpNestedProbe{ObjectMatrix m; float4 tail}`，验完已还原、`git diff` 干净）：**A** schema 嵌套字段 offset 0→16 → `[FAIL] attr[0]: API (m off=0 type=ObjectMatrix) != schema (m off=16 …)` 退 1 **不出文件**；**B** 被依赖 struct 排到引用者之后 → 交叉校验全 PASS 后 generation `[FAIL]` 点名顺序、退 1 不出文件；**C** `type="ObjectMatrixx"` → `[FAIL] unmappable schema type`（拼错名未被放行）。阳性：`RtmpNestedProbe` 正常生成且**编译过**（sizeof==80 / offsetof(tail)==64 assert 全过）。**跨文件嵌套实测**（临时 lights.slang 嵌 scene.slang 的 ObjectMatrix，经 import）：`findTypeByName` 与字段 `getName()` **两侧均跨模块可用**，生成物布局逐字段正确——后续阴影 VP / SceneHeader 的嵌套不用绕路。reflect 8 条全绿 / build / test 376 / checks 0 警告；两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`，本批零引擎改动、像素必然不变） |
| **G** | 护栏先行（§5 顺序红线）：**R5** 裸字节偏移棘轮（本表原写「R2」，落地时按 `check_shader_deps.py` 已有的 R1/R3/R4 现号续为 **R5**——R2 是退役空号，不复用以免被读成"复活退役规则"）+ 死 offset 常量检测 | ✅ **R5 棘轮已落地**（2026-10-12，零引擎改动）：存量 **130 条 / 19 个 .slang**（`scene.slang` 56 条为最，`meshlet.mesh.slang` 12、`cull.comp.slang` 8）钉进 `tools/checks/byte_offset_baseline.json`，按文件记只准减。变异三条实测**有牙**：① 加一条 `sceneData.Load4(12345u)` → ERROR「基线上升 5 > 4」+ `--strict` **退 1** ② 删一条真实偏移 → WARNING「可下调 0 < 1」+ 打印 `--update-byte-offset-baseline` 收账命令 ③ 基线改名 / JSON 写坏 / 缺 `files` 键 → ERROR「本次**未做任何对账**」+ `--strict` **退 1**（三条跑完均已还原，`git diff` 干净）。附带修掉一处**假绿**：`strip_comments` 两条正则版会把行注释里 `configs/*.lua` 的 `/*` 当块注释起点，`taa.frag.slang` 一次吞掉 224 行 → 该区新增偏移旧实现报 0 条。⚠ §5.1 **死 offset 常量检测不在本批**——零引擎改动 ⟹ 还没有"区已迁走"的常量可当阳性锚点，无锚点的判据立了就是空转，留到第一个迁移批（批 1）随迁随建 |
| **M** | mapping 按 SPV 声明过滤（§9.3）+ 启动面复测（必须 ≤ 基线 2.98s） | ✅ **已落地**（2026-10-12 本机）：mappings **6560→3722**（−43%，479 条目 958 份 SPV **全部解析成功、0 条目退回全烘**）、createTotal **2.93s→2.95s**（≤ 基线 2.98s；**持平**——本机 vkCreateShadersEXT 耗时由 shader 编译主导、mapping 条数边际成本小，本批收益是防住后续 +12 binding 的扩张面而非当下提速）；两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`），VVL 实开（loader 层插入日志）errors=0。变异两条实测**有牙**：A（恒空集）→ 首个条目 `vkCreateShadersEXT` 即报 VUID-VkShaderCreateInfoEXT-flags-11292「has no mapping for [Set 0, Binding 0, "modelVertexBuffer"]」⟹ **引擎非零退出**（比「画面变」更强：启动都起不来）；B（恒 nullopt）→ 像素逐位 = 锚 + mappings 精确回 6560 + 479 条「退回全烘」warn（fail-safe 方向 = 全烘且出声）。跑完均已还原 |
| **L** | 布局原地重排到 **256B 对齐**（§9.7）+ buffer `allocationAlignment` + 运行期对齐闸门 | ✅ **已落地**（2026-10-12 本机 AMD 8060S / RADV STRIX_HALO）：区基址全部改 `alignUp(前区尾, kSceneRegionAlign=256)` 派生，代表旧→新：对象矩阵表 128→**256**（新增命名常量 `kSceneDataObjectMatrixOffset`）/ 灯数组 262272→**262400**（lights_schema.lua 常量同步重反射）/ MVP 表 262800→**263168** / 材质表 524944→**525312** / 辐照度 721552→**721920** / 阴影 VP 721696→**722176** / IZB 722016→**722688** / TAA 722080→**723456** / SSR 722272→**723712** / 湿面 722368→**723968** / 簇区间表 722400→**724224**；`kSceneDataBytes` 787936→**789760**（定义改取尾区末端——对齐间隙让旧「求和式」失真）。运行期闸门（`SceneDataService::ensure`，形态先例 `checkMeshletDeviceFit`）实机读数：**minUniformBufferOffsetAlignment=4 / minStorageBufferOffsetAlignment=4 / maxUniformBufferRange=4294967295**（256 同时整除两者 ✓；不满足 = throw 点名，读数常驻 info 日志）。buffer 基址钉 `VmaAllocationCreateInfo::minAlignment = 256`（⚠ 本仓 VMA 3.4.0 字段名是 `minAlignment`，计划文本的 `allocationAlignment` 是 3.0–3.3 旧名）+ 创建后实测复核。两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`）；阳性对照（`readAoStrength` 的 @60 故意读 @76 = invVP c0.w 恒 0 = AO 旁路）→ 默认档 sha 变 `fe1b5e1e…`，还原复测回锚。R5 条数 130 不变（只改数值）。⚠ 踩坑一条：scene.slang 是反射管线输入，**改 .slang 后必须先 `just reflect`**，否则 `reflection_check` FATAL 拦停构建——阳性对照首跑因此拿旧 SPV 出图，「sha 不变」差点被读成判据失败（假绿的反向形态） |
| **1** | 湿面区（32B，1 消费者，无矩阵）双路打通全链路 | ✅ **已落地**（2026-10-12 本机 AMD 8060S / RADV STRIX_HALO）：消费端 = `scene.slang` `ConstantBuffer<WetParamsBlock> sceneWet`（binding 32，§9.5 定案表号），**shader 侧湿面字节字面量清零**（`readWetParams/readWetSpatial` 改字段读）；载体 = scene data 单 buffer 湿面区**子范围 UBO 描述符**（§9.2「拆描述符不拆 buffer」第一颗钉：`SceneDataService::ensure` 加领 Runtime 槽 "scene.wet"，与 scene.data 一次 `writeResourceDescriptorsEXT` 写两槽；buffer +`eUniformBuffer` usage，VUID-VkResourceDescriptorInfoEXT-type-11461）。契约链 = scene.slang struct → `scene_forward_schema.lua` vertexLayout → `just reflect` validator 交叉核对（PASS size=32 fields=2）→ 生成物 `WetParamsBlock`（自带 offsetof/sizeof assert）；`scene_data.cppm` 的湿面字面量 static_assert **删除**（钉的对象已不存在），test_wet_params 两源镜像换形 = 手抄字段偏移 0/16 ↔ 生成物 offsetof + `sizeof == kSceneDataWetBytes`。mapping 面 = `PipelineBindings::slotSceneWet` 对全部 hasSceneData 条目恒带上（批 M 的 SPV 声明过滤裁掉未声明者，C++ 不登记消费者）、`mappings[24]→[48]`（§9.7 红线：容量盖最坏全烘）。写侧本批**仍 memcpy**（`writeSceneStaging` 用 kSceneDataWet*Offset，与描述符基址同源同值——字面量第二源已消灭）。验收：**R5 130→128**（scene.slang 56→54，基线已 `--update-byte-offset-baseline` 收账、`--strict` 退 0）；两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`）；阳性对照 **A**（`readWetParams` 故意读 `sceneWet.spatial`）→ 默认档 sha 变 `ef1186a4…`、还原回锚；**B**（wet 描述符故意不写、槽保持全零）→ sha 变 `3acfc82e…`（不崩而画面变 = shader 真在消费 binding 32 的最强证据）、还原回两锚。build / test 376 / test-gpu 27 / checks 0 警告全绿 |
| 2..N | 按区推进（消费者少的先）：SSR → 簇区间表 → 材质表 → MVP 表 → 阴影参数 → 辐照度 → 灯数组 → 对象矩阵 → TAA → 阴影 VP → SceneHeader | 🔶 **辐照度 + 灯数组已落地**（批 2/3，2026-10-12 本机 AMD 8060S / RADV STRIX_HALO）：消费端 = `scene.slang` `ConstantBuffer<IrradianceBlock> sceneIrradiance`（binding 27，§9.5 定案表号），**9 条手写 Load4 + 基址字面量清零**（R5 **128→119**，scene.slang 54→45，基线已收账）；契约 = scene_forward_schema 登记 `IrradianceBlock`（stride 144，`sh` = "vec4[9]" @0——**数组字段形态首个消费者**，validator PASS size=144 fields=1，生成物 `std::array<glm::vec4,9>` + sizeof assert；scene_data.cppm 加 `sizeof(IrradianceBlock)==kSceneDataIrradianceBytes` 钉区大小）；载体 = Runtime 槽 "scene.irradiance" 子范围 UBO（三槽一次写）；mapping 恒带上（批 M 裁未声明者）；写侧改**按生成 struct 整块 memcpy**。验收：两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`）；阳性对照（`sh[3]` 故意读 `sh[4]`）→ 默认档 sha 变 `68ca2f21…`、还原回锚；build / test 376 / checks 0 警告全绿。⚠ 阳性对照首跑**再次**踩批 L 已登记的坑（改 .slang 未先 `just reflect` → reflection_check 拦停构建、旧二进制出图「sha 不变」= 假绿反向形态），当场识破重跑——坑是真的，别再第三次踩。 |
| 3 | 灯数组（**嵌套 struct 实战锚点**） | ✅ **已落地**（批 3，同日同机）：消费端 = `lights.slang` `ConstantBuffer<LightArrayBlock> lightArray`（binding 24，§9.5 定案表号），**3 条裸字节读清零**（R5 **119→116**，lights.slang 3→0，基线已收账）；契约 = lights_schema 登记 `LightArrayBlock`（count/pad + `lights` = "GpuLight[8]"——**批 R 嵌套 struct 能力的首个实战消费者**，生成物含 `sizeof(SceneForwardTypes::ObjectMatrix)==64` 跨模块断言）；`import scene` 从 lights.slang 移除（矩阵读改走 scene_forward 直取，DEP 表同步）；schema 常量 `kGpuLightStride`/`kSceneDataLightArrayBase` 保留为 C++ 侧锚点（test_reflection_layout + scene_data static_assert 消费）；载体 = Runtime 槽 "scene.lights" 子范围 UBO（四槽一次写）；写侧整块写（超 count 尾部由脏 staging 字节变 0，shader 只读 `lights[i<count]` ⟹ 像素中性）。验收：两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`）；阳性对照（`readGpuLight(i)` 故意读 `lights[i+1]`）→ 默认档 sha 变 `a87d1af3…`、还原回锚；build / test 376 / checks 0 警告全绿（ensure() 复杂度 17>15 → 对齐闸门拆为私有静态成员，文件级 static 会撞 `-WTU-local-entity-exposure`：接口单元 inline 成员函数不得引用 TU-local 实体）。其余区 ⬜ |
| **T0** | 表尾**毒化哨兵元素**护栏（§9.9 替代护栏本体，先于四表迁移） | ✅ **已落地**（2026-10-13 本机 AMD 8060S / RADV STRIX_HALO）：四张 per-object 表**区大小各 +1 个元素**（`kSceneData{ObjectMatrix,MvpTable,DrawTable,ClusterRange}Bytes = (容量+1)×stride`；🔴 可用容量常量仍是 4096，`static_assert(kSceneMatrixCapacity == 4096)` 单独钉住"区变宽 ≠ 多一个实例槽"）。毒值 `kSceneSentinelWord = 0xFFC00000`（float=NaN / uint=4290772992 / int=−4194304，三种元素类型都有毒；`0x80000000` 是 −0.0 **无毒**，测试里有反向用例）。落点 = 表基址 + 容量×stride（= 下标 4096 那一格、且是区最后一个元素）：矩阵表 **262400** / MVP **525568** / 材质 **722432** / 簇区间 **790528**。写侧 = `writeSceneSentinels` **只在 `Renderer::createSceneStaging` 分配每个 staging slot 后调一次**，每帧 `kSceneDataBytes` 全量 `vkCmdCopyBuffer`（主 CB ⑥.5 与 `GpuCull::recordCullPass` 两处都是全量）把毒值带进 device buffer ⟹ **零每帧代价**。🔴 顺带修掉一处**会拆护栏的既有写法**：`writeClusterRangeRegion` 的 memset 原长 `kSceneDataClusterRangeBytes - usedBytes`，区宽 +1 之后会把哨兵每帧抹成 0 ⟹ 清零段改为**止于哨兵之前**（`tests/test_meshlet_ranges` 的 ②/门关两条同步换钉）。**布局连带搬家（本批真实代价，比计划文本大）**：区变宽 ⟹ 其后所有区基址按 256 台阶后移 —— 灯数组 262656 / MVP 263424 / 材质 525824 / 辐照度 722688 / 阴影 VP 722944 / splits·texel·params·atlas 723200/7216/723232/723248 / IZB 723456 / PCSS 723712 / EVSSM 723968(+16/+24) / TAA 724224 起六段 724288/724352/724368/724384/724400 / SSR 724480(+64/+80) / 湿面 724736(+16) / 簇区间 724992，`kSceneDataBytes` 789760 → **790544**（+784B）；同步面 = scene.slang 18 条字面量 + light.frag IZB 1 条 + lights_schema 常量（重反射）+ 三个测试的手抄字面量 + 全部提及旧号的注释（脚本单遍映射，非链式替换）。验收：两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`，**布局整体搬家而像素一 bit 不动 = 本批全部判据**）；🔴 **有牙变异**（`readObjectMatrix` 下标 `+4096u` 正好读进哨兵）→ 门关 `01292d81…`、默认 `e8119df6…` **两档都变**，还原后回两锚（这条排掉"哨兵没被毒化 / 位置不对 / 每帧被覆盖"三种失效）；新增 `tests/test_scene_sentinel`（8 例 / 变异红线七条：毒值改 0 或 0x80000000、哨兵偏移 ±1 格、容量偷改 4097、区宽忘 +1、memset 写回整区、少写/多写窗口）；build / test **384** / test-gpu 27 / checks 0 警告 / `just smoke --sync` **hazard 0 errors 0** 全绿 |
| **T1** | 逐对象四张表（世界矩阵 / MVP / 绘制属性 / 簇区间）从 `ByteAddressBuffer` 字节偏移迁到 `StructuredBuffer<T>`（binding 23 / 25 / 26 / 33 = §9.5 定案表号，逐表对应 矩阵 / MVP / 绘制 / 簇区间） | ✅ **已落地**（2026-10-13 本机 AMD 8060S / RADV STRIX_HALO）：消费端 = **新模块 `rgl/shaders/scene_tables.slang`**（`import scene;` 取结构体 + 四条 `StructuredBuffer<T>` + 四个读取函数**沿用原名** `readObjectMatrix`/`readObjectMvp`/`readObjectDraw`/`readObjectClusterRange` ⟹ 14 个消费者只多一句 `import scene_tables;`，调用点零改动）。🔴 **为什么不并进 scene.slang**：它是反射管线输入单元（结构体声明必须留在那儿 = 布局唯一真相源），且历史上被两个 compute 整份 import 过（自带 binding 3 撞号）——把 `StructuredBuffer` 塞回去等于重埋那颗雷；`public import scene;` 在本 Slang 版本被拒（`E31201 modifier not allowed`），**import 非传递** ⟹ 消费者要 `import scene;`（取类型名）+ `import scene_tables;`（取函数）两句。C++ 侧 = `SceneDataService::ensure` 描述符 **4 槽 → 8 槽**（四张表各领一个 Runtime 段槽，`eStorageBuffer` 子范围 `{基址 + 区基址, 区长度**含哨兵**}`；领槽与写描述符改表驱动，防「加了槽忘写描述符」）+ `PipelineBindings` 四字段 + `RenderPipeline` 四条 `add()`（恒带上，未声明的由批 M 过滤裁）+ `ShaderRegistry::fillSceneRegionSlots` 收掉 15 处装配点的重复段（顺带把 `shader_registry_impl.cppm` 拉回 500 行红线内）。**两个 compute 的私有 binding 布局重排**：`cull.comp` 删 binding 1（全区字节读）、`countBuf` 3→**4**；`meshlet_cull.comp` 删 binding 2、`clusterVisible` 3→**4** —— 因为 `import scene;` 之后 scene.slang 的 `sceneData` 声明占着 binding 3，同占显式 binding = slangc `warning[E39001] explicit binding overlap`（sceneData 未用到 → 实测**不进 SPV**，但警告恒出声，不赌）；mesh 侧读标志表是**另一条**管线布局（binding 21），挪号只同步两处 `makeMappedCompute` 的 binding→槽表。DEP 表按传递闭包补 **26 条**（脚本算，R1 零漏登）。**验收**：两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`，改前改后各测一次）；🔴 **四条阳性对照**（每表一条，读取下标 `+1u`）默认档 sha 逐一变：矩阵 `e26b73a0…` / MVP `e979ed0c…` / 绘制 `bedb0888…` / 簇区间 `9452587b…`，四条还原后均回锚；R5 裸字节偏移 **116 → 102**（−14：scene.slang −8 / cull.comp −3 / meshlet_cull.comp −3，基线已收账）；其中**scene data 裸偏移 64 → 50**；stride-check `shaders 内实际使用` **0 → 4**、ERROR 0（R3 放行面 = 反射契约类型，R4 逐次构建对账 `ArrayStride` == 反射 `sizeof`：ObjectMatrix 64 / ObjectDraw 48 / ObjectClusterRange 16）；build / test **384** / test-gpu 27 / checks 0 警告 / `just smoke` errors=0 / `just smoke --sync` **hazard 0 条 errors 0**；🔴 因动了 cull 的 binding，`cull_replay` **三档各自** `--sync` 冒烟：cpu / gpu_sync / gpu_lagged 全部 hazard 0 errors 0。⚠ 环境事实：`scene_tables.slang` **不是**反射输入，改它只需 `just build`（不必 `just reflect`）；`scene_tables.slang` 单独编译报 `E57004 no exported symbols`（无入口点，它只作为 DEP 被消费） |
| **S-1** | 阴影**光空间 VP 区**（4 槽 × mat4 = 256B，@722944）从 `ByteAddressBuffer` 四条手写 `Load4` 迁到 `ConstantBuffer<ShadowViewProjBlock>`（binding 28 = §9.5 定案表号） | ✅ **已落地**（2026-10-13 本机 AMD 8060S / RADV STRIX_HALO）：消费端 = `scene.slang` `ConstantBuffer<ShadowViewProjBlock> sceneShadowViewProj`（binding 28），`readShadowViewProj` 四条手写 `Load4(base + cascade*64)` → **一行数组下标读**，返回类型仍是 `ObjectMatrix`（列主序 4 列，元素步长 64 = 原 `cascade*64`）⟹ 五个消费者（`shadow.slang` / `shadow.mesh` / `shadow_meshlet.mesh` / `izb_mask.mesh` / `light.frag`）**调用点零改动**。契约 = scene_forward_schema 登记 `ShadowViewProjBlock`（stride 256，唯一属性 `viewProj` 写 `"ObjectMatrix[4]"`）→ 生成物 `std::array<ObjectMatrix, 4>` + `sizeof == 256` assert，反射交叉校验 `[PASS] size=256 fields=1`。**这是「契约 struct 的数组字段」首个消费者**（批 R 登记的 `ObjectMatrix` 在此第一次作为**数组元素**嵌进别的契约 struct；批 R 那条注释列的四个用途之一兑现），发射顺序必须排在 `ObjectMatrix` 之后（`generateCppmFromSlang` 的发射顺序 [FAIL] 会点名）。C++ 侧 = `SceneDataService` 区槽 **8 → 9**（新增私有常量 `kRegionSlotCount` 给四张 `std::array` 取尺寸 ⟹ 加区只改一处，**漏写描述符行 = 编译失败**，而不是运行期读到 heap 槽里的上一个描述符）+ `shadowViewProjSlot()` + `PipelineBindings::slotSceneShadowViewProj` + `add(28, …)`（恒带上，未声明者由批 M 过滤裁）+ `fillSceneRegionSlots` 一行。写端**逐字节不变**（原本就是一条 `memcpy`，本批补 `static_assert(sizeof(源数组) == kSceneDataShadowMatrixBytes)`）。死同步点清理：删 `static_assert(kSceneDataShadowMatrixOffset == 722944)`（shader 字面量已消失，两侧手写同步点没有另一端了），换成两条**同源** assert：`sizeof(ShadowViewProjBlock) == kSceneDataShadowMatrixBytes`、`kSceneShadowCascadeCount == 数组长度`。**布局零搬家**：722944 + 256 = 723200 恰是下一区现基址，`alignUp` 后其后各区逐字节不动。**验收**：两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`，改前、变异还原后各一次）；🔴 **阳性对照**（级联号 `+1u` → 读到相邻槽，而 [1..3] 恒为单位阵 ⟹ 几何出裁剪体、阴影图空）两档**都变**：门关 `6b447a07…` / 默认 `51c6f9be…`，还原后回两锚；R5 裸字节偏移 **102 → 99**（scene.slang 37 → 34，基线已收账）；build / test **384** / test-gpu 27 / checks 0 警告 / `just smoke` errors=0 / `just smoke --sync` **hazard 0 条 errors 0**（日志 VUID 行 0）。binding 28 此前无任何 `.slang` 显式占用（grep 实测）⟹ 无批 T1 那种 compute 挪号问题 |
| **S-2** | 阴影**采样参数 7 段小 vec4**（splits/texel/params/atlas + IZB + PCSS + EVSSM 2×vec4，原各占 256 台阶共吃 1024B）合并成一个 `ShadowParamsBlock`（128B）迁 `ConstantBuffer`（binding 29 = §9.5 定案表号） | ✅ **已落地**（2026-10-13 本机 AMD 8060S / RADV STRIX_HALO）：消费端 = `scene.slang` `ConstantBuffer<ShadowParamsBlock> sceneShadowParams`，**八个读取函数全部改字段读**（`readCascadeSplits` / `readCascadeTexelSizes` / `readShadowParams` / `readShadowAtlas` / `readShadowPcss` / `readShadowEvssm` / `readShadowEvssmExponent`=`.evssmConst.x` / `readShadowEvssmFootprint`=`.evssmConst.z`），并**新增 `readShadowIzb()`** 顶掉 `light.frag` 里手抄的 `sceneData.Load4(723456u)`（那条 R5 与它的 `kSceneDataIzbOffset == 723456` 手写同步点一起消失）。契约 = scene_forward_schema 登记 `ShadowParamsBlock`（stride 128，8×vec4）→ 反射 `[PASS] size=128 fields=8`，生成物**逐字段 offsetof assert 全发**。字段序 = 合并前的物理序（前四段本就同区连排 ⟹ 逐字节同）。写端 = `writeSceneStaging` **按生成 struct 整块 memcpy**（designated init 名字绑定 ⟹ 写错字段是编译错误），IZB 段的值上提到块里一起写（原本就是无条件写，门关 = 全 0 不变）。**删掉 9 个字段级基址常量**（Shadow{Splits,Texel,Params,Atlas}Offset / ShadowAuxBytes / Izb{,Bytes} / Pcss{,Bytes} / Evssm{,Bytes} / EvssmConstOffset），本文件只留区基址+容量；`checkAlignmentGate` 的 UBO 候选表同步换条目。🔴 **本批真实代价 = 布局搬家 −768B**（合并省掉 3 个 256 台阶对齐间隙）：TAA 724224→**723456**（六段 →723520/723584/723600/723616/723632）/ SSR 724480→**723712**（params →723776、gate →723792）/ 湿面 724736→**723968**（+16→723984）/ 簇区间表 724992→**724224**（表尾哨兵 790528→**789760**）/ `kSceneDataBytes` 790544→**789776**；搬家面 = 代码与配置里 **161 处**字面量/注释（脚本单遍映射 139 + 补第二遍 22），**docs 台账里的历史数字不改写**（那是账目不是现状）。⚠ 踩坑一条（写进脚本纪律）：`\b` 词边界**不认数字+字母**——`724224u` / `790544B` 这类带后缀的第一遍全漏，第二遍得用 `(?<!\d)旧(?=[uB])`，且此时新值 724224（簇区间新基址）与旧值 724224（TAA 旧基址）**同值不同义**，链式改第二遍就把新基址串回旧值——单遍映射 + 后缀分遍是硬要求。**验收**：两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`，改前、变异还原后各一次，**布局整体搬 768B 而像素一 bit 不动**）；🔴 **阳性对照**（`readShadowAtlas()` 故意读同块相邻字段 `.pcss`）两档**都变**为同一张 `3442f7b0…`（两档收敛 = 图集映射坏掉把阴影项压成常量、meshlet 开关的唯一画面差异被抹掉；解压后非零字节比与基线同 = 不是黑屏），还原后回两锚；R5 裸字节偏移 **99 → 90**（scene.slang 34→26、light.frag 4→3，基线已收账）；build / test **384** / test-gpu 27 / checks 0 警告 / `just smoke` errors=0 / `just smoke --sync` **hazard 0 条 errors 0**（日志 VUID 行 0）。⚔ **门关红线盖不到的三条变体路额外实跑**：izb / pcss / evssm 是 `-D` 变体，默认档 SPV 根本不读那三段 ⟹ 两档锚点测不到它们，而本批恰好搬了它们的字节位置——`just shot --set` 的 `SET_TARGETS` 白名单不含 shadow 文件（拼错目标会新建没人加载的文件 = 静默不生效，白名单存在的理由），故临时改 `configs/shadow.lua` 重编跑两跑：`mode=evssm + izb.on=true`（日志 `shadow: mode=evssm izb=true` + `izb: 开门 grid=512 … IZB_CONSUME 变体`）与 `mode=pcss + izb.on=true`，两条 errors=0、图非退化，跑完 `git checkout` 还原配置。⚠ 工具面预留：把 `shadow` 加进 `SET_TARGETS`（含 patch_lua 对 `return {` 根块的键定位）没做→ **已落地**（2026-10-13）：白名单不再手抄，改为从 `configs/*.lua` **磁盘派生**（`tools/smoke.py::set_targets`，正是本行「白名单 + 配置文件清单同源才拦得住」那句话的兑现）；`--set 'shadow:mode="pcss"'` 实测引擎回显 `shadow: mode=pcss`。patch_lua 的根块定位本来就够用，当年的唯一卡点就是白名单本身 |
| **S-3** | SSR 尾区（viewProj mat4 + params vec4 + gate vec4 = 连续 96B，@723712）迁 `ConstantBuffer<SsrParamsBlock>`（binding 31 = §9.5 定案表号） | ✅ **已落地**（2026-10-13 本机 AMD 8060S / RADV STRIX_HALO）：消费端 = `scene.slang` `ConstantBuffer<SsrParamsBlock> sceneSsr`（binding 31），`readSsrViewProj` / `readSsrParams` / `readSsrGate` 三个读函数改字段读（`viewProj` 直接返回 `ObjectMatrix`，列主序与旧手写四行 `Load4` 逐字节同）。契约 = scene_forward_schema 登记 `SsrParamsBlock`（stride 96 = 64 + 16 + 16）→ 反射 `[PASS] size=96 fields=3`，生成物逐字段 offsetof assert 全发。**本批零布局变更**（SSR 区自批 15.1/15.3 起本就连续）：`kSceneDataBytes` 仍 789776，其后湿面 / 簇区间表逐字节不动。删掉 `kSceneDataSsrParamsOffset` / `kSceneDataSsrGateOffset` 两枚字段级常量（`kSceneDataSsrBytes` 改 `sizeof(SsrParamsBlock)`，仍被 `test_ssr_params` 当 stride 断言）；`SceneDataService` 区槽 **10 → 11** + `ssrSlot()` + `add(31, …)` + `fillSceneRegionSlots` 一行。写端 = `writeSceneStaging` 整块 `memcpy`，mat4 按**列**摊进 `ObjectMatrix` 四个具名 float4（§9.4：不用 `float4x4` 字段）。**测试同步换形态**：`test_ssr_viewproj` / `test_ssr_params` 从「手抄字节偏移 + `Load4` 语义」改成 `test_wet_params` 那套「手抄字段偏移 0/64/80 + `offsetof(SsrParamsBlock, …)` 双钉」——手抄那份是契约（类型化后仍会漂），生成物那份是机器真相。⚠ 本构建 `alignof(glm::vec4) == 4` ⟹ `alignof(SsrParamsBlock) == 4`，字节形状断言按 `sizeof == sizeof(glm::mat4) + 2 * sizeof(glm::vec4)` 写，**别写 `alignof == 16`**。顺带清掉两处失效字节注释（`configs/runtime.lua` 的 `ssrParams(@723776) / ssrGate(@723792)`、`shader_registry.cppm` 的 `@723712 ssrViewProj`）→ 改指字段与写侧真相源。**R5 棘轮 90 → 85**（scene.slang 26 → 21，基线已收账）。**验收**：两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`，改前、变异还原后各一次）；🔴 **阳性对照**（`readSsrGate()` 改读同块相邻字段 `sceneSsr.params`）两档**都变**：门关 `acb24ee5…` / 默认 `e4508d5a…`，还原后回两锚；build / test **384** / test-gpu 27 / checks 0 警告 / `just smoke` errors=0 / `just smoke --sync` **hazard 0 条 errors 0**。⚔ **锚点盖不到的两个字段另找覆盖**：`ssr.enabled` 默认关 ⟹ 锚点里只有 `gate` 被湿面身份路径读，`viewProj` / `params` 走不到屏幕（对它们做变异两档都不会变 = 画面不可见，不是漏读）。故补两跑：`just shot --ssr`（SSR 开档）**PASS errors=0**（apply 1280×720 = 渲染网格 / trace 640×360 = 半网格，出图 `c48cc59e…`）+ 手搭 `RGL_CONFIG_DIR` 的 `ssr.enabled=true` 档在 `VK_LAYER_VALIDATE_SYNC=1` 下真出一帧（**0 error / 0 SYNC-HAZARD**，RT 两行齐）。⚠ **工具坑（未修，非本批范围）**：`just shot --set ssr.enabled=true` 必挂 smoke 自己的「默认档（ssr.enabled 关）却建了 ssr RT ⟹ 门关不拢」护栏——该护栏只看具名旗标 `args.ssr`，不认 `--set` 注入的开门（`tools/smoke.py` 的 `if not args.ssr and (ssrRt or ssrTraceRt)`）；SSR 开档要用 `--ssr` |
| **S-4** | TAA 尾区（prevViewProj / motion / params / prevContentShift / inputKernel / clipControl，连续 192B @723456）迁 `ConstantBuffer<TaaParamsBlock>`（binding 30 = §9.5 定案表号） | ✅ **已落地**（2026-10-13 本机 AMD 8060S / RADV STRIX_HALO）：消费端 = `scene.slang` `ConstantBuffer<TaaParamsBlock> sceneTaa`（binding 30），**八个**读取函数全部改字段读（readTaaPrevViewProj / readTaaMotion / readTaaParams / readTaaPrevContentShift / readAoAppliedPreTemporal / readTaaDiagReject / readTaaInputKernel / readTaaClipControl）——**只换读法、一个算术 bit 未动** ⟹ `velocity_field.cppm` 的 CPU 侧镜像与其单测无需改（实测 5 例 / 442 断言全绿）。契约 = scene_forward_schema 登记 `TaaParamsBlock`（stride 192 = 2×ObjectMatrix + 4×vec4）→ 反射 `[PASS] size=192 fields=6`；**「契约 struct 嵌契约 struct」非数组形态的第二个消费者**（§9.4：矩阵一律 `ObjectMatrix`）。零布局变更（六段自批 13.11 起本就同区连排）。C++ 侧 = 区槽 11 → 12 + `taaSlot()` + `add(30, …)` + 写端整块 memcpy。🔴 **本批判据重心必须换档**（这是它区别于其它批的地方）：TAA **默认关**（`aa` 三态选路，关 = 连 jitter 注入与全屏 blit 都不发生）⟹ **两档锚点对 TAA 区零覆盖**。这不是假设，是实测：施加两条变异后两档锚点**仍逐位 = 锚**（A `readTaaPrevContentShift` 改读 `.params` → `722574f1…`；B `readTaaPrevViewProj` 改读 `.motion` → `3ce8a6f4…`，两档锚点全程不动）——**锚点绿 = 什么都没测到**，阳性对照必须在真执行该读的档做。故本批判据 = 自量一份 aa=taa 档基线（`just shot --aa taa`，两次跑出同一 `823d5442…` = 该档单帧确定）+ 上面两条变异在该档离开基线 + `just smoke --aa taa` 与 `--aa taa --sync` 各一次 errors=0 hazard=0。⚠ 顺带删掉六个 TAA 字段级基址常量并把它们的长注释**改写成契约字段名**保留（`/**` → `/*` + 标题换 `契约字段 TaaParamsBlock::<field>`），四处指向已删常量的失效注释指针就地改指字段（scene.slang / scene_context.cppm / scene_config.cppm / tools/imgqa 三份 .h）。**R5 棘轮 85 → 73**（scene.slang 21 → 9，基线已收账）；build / test 384 / test-gpu 27 / checks 0 警告 / `just smoke` errors=0 / `just smoke --sync` hazard 0 errors=0（默认档 + aa=taa 档各一次）。⚠ TAA 的 imgqa 用例有**已知红绿漂移**（时域档不做位比闸门 → skill `rgl-verify`），本批不追 |
| **S-5** | **头区**（light 48B + envParams vec4 + skyInvVP mat4 = 连续 128B @0）迁 `ConstantBuffer<SceneHeaderBlock>`（binding 22 = §9.5 定案表号，**最后一个迁出的区**） | ✅ **已落地**（2026-10-13 本机 AMD 8060S / RADV STRIX_HALO）：消费端 = `scene.slang` `ConstantBuffer<SceneHeaderBlock> sceneHeader`（binding 22）——改写 readSceneLight / clipToWorld / readAoStrength，新增 readEnvIntensity / readEnvMipRange / readExposure / readSceneEyePos / readSkyInvViewProj。**八处手写头区字节读清零，跨 7 个文件**（scene.slang Load3 0/12/24/36 + Load4 64/80/96/112 + Load 60 / sky.slang Load4 ×4 / env.slang Load 48,52 / light.frag Load 48,52 / ssr_trace.frag Load 48,52 / ao.frag Load3 12 + Load 60 / tonemap.frag Load 56 / shadow.slang Load4 64,80）。契约 = scene_forward_schema 登记 `SceneHeaderBlock`（stride 128：`light` = "SceneLight" @0 / `envParams` vec4 @48 / `skyInvVP` = "ObjectMatrix" @64）→ 反射 `[PASS] size=128 fields=3`。`envParams` 把三个独立标量收进一个 vec4（x=envIntensity y=envMipRange z=exposure w=aoStrength），写端由**五段 memcpy**（light @0 / vec2 @48 / float @56 / float @60 / mat4 @64）变**一段整块 128B memcpy**（designated init 名字绑定 ⟹ 写错字段是编译错误），字节形态逐位同。零布局变更。C++ 侧 = 区槽 12 → 13 + `headerSlot()` + `PipelineBindings::slotSceneHeader` + `sceneRegions[]` 一条 + `fillSceneRegionSlots` 一行；新增 `kSceneDataHeaderOffset = 0`。⚠ **`offsetof` 不能进引擎 GMF**（样式规则 B2 禁带 `<cstddef>`，`meshlet_hand_impl.cppm` 同款处置）⟹ 字段级对账**传递**成立：生成物钉 `offsetof(字段) == 48/64`、`scene_data.cppm` 钉 `kSceneData{Env,Sky}Offset == 48/64`、本文件只钉容量 `sizeof(SceneHeaderBlock) == kSceneDataHeaderBytes`。⚠ 两条命名撞车（都别再撞）：`env.slang` 已有返回 `EnvParams` 的 `readEnvParams()` ⟹ scene.slang **不能**再提供 `float2 readEnvParams()`（同名不同返回类型 = 编译错误），头区 env 因此走两个标量函数；`tonemap.frag` 的本地 `readExposure()` 与模块新函数同名同签名 ⟹ 删本地那份（`readAoStrength` 抬进模块的同一条先例）。🔴 **`sceneData` 的 `.slang` 消费者归零**：剥注释后 `rgl/shaders/**.slang` 里 `sceneData` 命中 0 条（用 `check_shader_deps.py` 自己的 `strip_comments` 算，不是 grep 注释行）⟹ N+1 前提由机器确认。**R5 棘轮 73 → 49**（−24：scene.slang 9→0 / sky.slang 4→0 / light.frag 3→1 / env.slang 2→0 / ao.frag 2→0 / shadow.slang 2→0 / ssr_trace.frag 2→0 / tonemap.frag 1→0）。**验收**：两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`，改前、变异全还原后各一次）；🔴 **阳性对照 7 条**（只换取材点不改算术）出厂默认档 sha 逐一离开锚：envParams.z→y（曝光）`2e05cef8…` / envParams.x→w（env 强度）`dd220dba…` / envParams.w→z（AO 强度）`553cca6e…` / `clipToWorld` c0↔c1 `696dac26…` / shadow.slang camRight↔camUp `3c62a894…` / `light.eyePos`←envParams.xyz `ab0fc1d1…` / 头区描述符 address 指向 TAA 区 `1d309a85…`。🔴 **`light.direction` / `.color` 两条在锚点档 SAME = 判据选错档，实测裁决为「覆盖缺失」不是载体故障**：临时日志实测「实际注册 Light entity = 1」⟹ `readLightCount() > 0` ⟹ deferred `light.frag:249` 与 forward 三个 IBL 风格的 `if (n == 0)` 头单灯回落分支**从不执行**（`model_pbr` / `model_phong` 虽无条件读，默认场景不走这两个风格）。把 Light entity 强制为 0 另开一档：该档基线 `94e8cbbd…` → 同档施加变异 `859f1856…` **变了** ⟹ 载体把 @0 的 direction 送到了着色器，「载体没接上」被排除。⚠ 踩坑**第三次**登记（批 L / 批 2 已两次）：`scene.slang` 是反射输入，变异改它**必须先 `just reflect`**，否则 `reflection_check` FATAL 拦停构建 ⟹ 旧二进制出图 ⟹「sha 不变」是**假绿的反向形态**；首轮 6 条变异里 5 条因此白跑一轮，驱动器已把 reflect 固化成一步。build / test 384 / test-gpu 27 / checks 0 警告 / `just smoke` errors=0 / `just smoke --sync` hazard 0 errors 0 |
| **N+1** | 删尸体 + **永久护栏**：`sceneData` 声明与 `[[vk::binding(3,0)]]`、binding 3 的**全区**描述符与 heap 槽、`PipelineBindings::slotSceneData` 与 `sceneRegions[]` 的 binding 3 条目、`ensure()` 的槽号返回值、两枚子字段常量；新增护栏 **R6** | ✅ **已落地**（2026-10-13 本机 AMD 8060S / RADV STRIX_HALO）：**前置实测**：`git grep -n sceneData -- '*.slang'` 剩余命中**全部在注释里**（历史散文），唯一活代码一行 = 待删的 `scene.slang` 里 `ByteAddressBuffer sceneData;`；另 `slang_api_spike/test_struct.slang` 里有一处真声明，那是 slangc C API 的独立探针、不在 `rgl/shaders/` 扫描面内（R6 因此也不会误伤它）。**删除清单**：① `scene.slang` 的 `[[vk::binding(3,0)]] ByteAddressBuffer sceneData;`；② `SceneDataService` 的 `allocRuntimeSlot("scene.data")` + `sceneDataSlot` 成员 + `writeRegionDescriptors` 三张表的首行（区槽 **13 → 12**）；③ `PipelineBindings::slotSceneData` + `sceneRegions[]` 的 `{3, …}` 条目 + 15 处装配点的赋值；④ `ensure()` 返回类型 `uint32_t` → `void`（槽号再无去处；**buffer 本体与 12 条子范围描述符一条不少**，`getBuffer()` 仍是每帧 staging copy 的目标）；⑤ 两枚子字段常量 `kSceneDataExposureOffset` / `kSceneDataAoOffset`（它们表达的是 `envParams` 这个 vec4 的第 3/4 个 float，现在由契约字段 `.z` / `.w` 说 ⟹ 留字节数 = 双源），头区 static_assert 链因此改写成 vec4 整体跨度。🔴 **保留清单（逐个确认过引用，不是"看着像常量就删"）**：`kSceneDataHeaderOffset`（写端 memcpy 目标）· `kSceneDataEnvOffset` / `kSceneDataSkyOffset`（互为派生 + 钉契约字段偏移 + `test_shadow_forward` 字节级镜像的取材基址）· `kSceneDataHeaderBytes`（派生 `kSceneDataObjectMatrixOffset`）· `kSceneDataLightArrayBytes` / `kSceneDataIrradianceBytes`（**下一区基址的派生式**，只在 scene_data.cppm 内出现 ≠ 死）· 全部区基址/容量/哨兵偏移 · `kSceneDataBytes`。失效 read 函数只找到**一个** `uvFromWorld`——它是通用投影助手（与仍在用的 `uvFromClip` 成对，且 `velocity_field.cppm::uvFromWorld` 的 CPU 镜像以它为口径出处），**不是 scene data 读取面** ⟹ 本批不删，理由记在这里免得被当成漏删。🔴 **永久护栏 R6**（`tools/check_shader_deps.py`，挂号 → `docs/风格检查规则.md` §6.3）：任何 `.slang`（剥注释后）出现 `ByteAddressBuffer sceneData` 或 `sceneData.Load*/Store*` = **ERROR**；自检先于判据（扫描面里必须看见 `scene.slang`，否则 ERROR「本次未做任何对账」）。三条变异实测有牙：A 加回声明 + 一条 `Load4(0u)` → **两条 R6 ERROR**（+ R5 同时报基线上升）`--strict` 退 1；B 扫描面置空 → ERROR「未做任何对账」退 1；B2 扫描面换成错目录（里面真有一行 `sceneData` 声明）→ 同样 ERROR 退 1，自检**先于**判据出声。三条跑完均还原、基线重跑回绿。**验收**：两档锚点逐位命中（门关 `54e40665…` / 默认 `95edc8cc…`）；剩余 R5 逐条归属见 §9.10；build / test 384 / test-gpu 27 / checks 0 警告 / `just smoke` errors=0 / `just smoke --sync` hazard 0 errors 0（本批动了描述符下发面：13 行 → 12 行）。⚠ **射程外**：`cull.comp` / `meshlet_cull.comp` 当年为躲 binding 3 撞号把自己的私有 binding 挪到 4（批 T1），现在 sceneData 没了、那个约束消失，**但没挪回去**——挪回去是另一件事（要重跑 cull 三档 sync 冒烟），本批不动 |
| 末 | `cull.comp.slang` 遗留偏移（C3，纯遗留不阻塞） | 🔶 **scene data 那一条已还**（批 T1：对象矩阵表改 binding 23 `StructuredBuffer<ObjectMatrix>`，`kSceneMatrixBase = 256u` 手抄基址删除）。**剩余** = `cullTable`（60B）/ `sequences`（120B）/ `countBuf` 的偏移，那是 `gpu_cull.cppm` **自己的**私有契约、不是 scene data，不在本计划射程（要收是「给 cull 契约上反射」另一件事） |

🔴 **护栏（批 G）先于任何迁移批**（§5 顺序红线："先建护栏再迁移，顺序反了护栏永远补不上"）。
🔴 每批收账必须写回本节状态列 + `docs/还债实测数据.md`，**数字绑机**（AMD 8060S / RADV
STRIX_HALO / Mesa 26.2.3）。

### 9.9 §5.3 GPU-AV 冒烟臂：**否决**（实测两条独立障碍）+ 替代护栏

结论先说：**GPU-AV 在本项目架构下不可用**，不是"没配好"。别再试第三次，两条障碍都是实测：

**障碍一：环境变量根本开不起来**（先例 syncval 的 `VK_LAYER_VALIDATE_SYNC=1` 走不通）。
本机 VVL 实测无效的写法（每种都以 `createTotal` 不变为准据判为 no-op）：
`VK_LAYER_ENABLES=GPU-Assisted`（**连乱码值都不报错** = 该变量不被解析）、
`VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT=1`、`VK_KHRONOS_VALIDATION_ENABLE={GPU-AV|gpu_av|GPU-Assisted}`、
`VK_LAYER_SETTINGS_PATH`（`khronos_validation.enable = GPU-AV` 与 ini 头两种写法）。
✅ **唯一有效形态 = 代码里挂 `vk::ValidationFeaturesEXT` pNext 到 instance create**
（`eGpuAssisted` + `eGpuAssistedReserveBindingSlot`；rlvk 需补
`ValidationFeatureEnableEXT` / `ValidationFeaturesEXT` 两行 `using`）。

**障碍二：开起来后被 `VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT` 顶死**。VVL 原文：
> `vkCreateShadersEXT(): pCreateInfos[1] Internal Error, GPU-AV is being disabled.`
> `Unable to instrument shader using VkIndirectExecutionSetEXT validly … because of GPU-AV's
> usage of VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC (If you don't need
> VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT, turn it off).`

⟹ GPU-AV 与 DGC 的 indirect-bindable shader **互斥**（`rgl/shader.cppm` 两处 flags 是无条件的）。

**障碍三（决定性，也是最贵的一条）**：把两处 `INDIRECT_BINDABLE` 摘掉后 GPU-AV **确实跑起来了**
（`createTotal` 2.95s → **5.63s** = 插桩代价，同时是"它真的在"的判据），
但**故意越界读它一条不报**：把 `scene.slang::readAoStrength` 改成
`sceneData.Load(4000000u)`（buffer 只有 789760B）——截图 1376KB → 1360KB **证明该读真的执行了**，
`errors=0`、日志零条越界报告。

机制解释（**假设**，两条观察收敛）：VVL 自己那句
"GPU-AV instruments all shaders at vkCreateShadersEXT time, this means there are embedded
descriptors bound that we can't detect if needed or not later" ⟹ descriptor heap 下它拿不到
描述符的 buffer 边界。**这与 `docs/还债总账.md` 债-75 的「shader 侧 buffer 访问 VVL 无从归因」
是同一条机制的两处独立观察**（那边是 syncval 删 barrier 变异 0 条，这边是 GPU-AV 越界读 0 条）。

**为什么不摘 flag 硬凑一臂**：摘掉 `INDIRECT_BINDABLE` 后测的**不是交付形态**（DGC 路整条不成立），
"另一条臂" 的判据强度低于它带来的维护面；且它仍抓不到越界（障碍三），等于只付代价不拿收益。

**替代护栏 = 表尾毒化哨兵元素**（本计划采用，随第一个 `StructuredBuffer` 表批落地）：
各区之间现在**紧邻**，越界读读到的是**邻居区的合法数据** = 静默错值。给每张表尾追加
**一个元素**，内容由 `ensure()` 一次性写成**毒化模式**（NaN 填充，非零、不参与每帧写），
越界读 ⟹ NaN 进着色器 ⟹ **锚点必炸**。
- 判据（有牙）：把某 read 的下标临时 +1 → 两档锚点必须变；还原回锚。
- 代价：每表 +1 元素（矩阵表 64B）、`kSceneDataBytes` 略增、零每帧代价。
- 它抓不到"界内读错元素"（那本来就不是越界问题，靠逐位锚点 + 阳性对照）。

### 9.10 剩余 R5 裸字节偏移**逐条归属**（批 N+1 收账，2026-10-13）

§2 的成功判据是「scene data 的偏移写不出来了」。要证明这句话，光报一个总数 49 不够——
**必须逐条说出每一条属于哪个 buffer**，否则"scene data 一条不剩"只是断言。

下表由 `tools/check_shader_deps.py` 自己的扫描器现算（`scan_byte_offsets` 的命中行 → 接收者
变量名 → 同文件 `[[vk::binding(N)]]` 声明行现取 binding 号），**不是手抄**。总条数与
`tools/checks/byte_offset_baseline.json` 的 `total` 逐位相等（49）。

| 文件 | 条数 | 接收者（= 哪个 buffer） |
|---|---|---|
| `rgl/shaders/meshlet.mesh.slang` | 12 | meshletVertexPool（binding 17）×4 / meshletClusters（binding 18）×4 / meshletRemap（binding 20）×2 / clusterVisible（binding 21）×1 / `buf`×1（= helper `loadU8x3(ByteAddressBuffer buf, …)` 的形参，实参 = meshletClusters） |
| `rgl/shaders/shadow_meshlet.mesh.slang` | 6 | meshletClusters（binding 18）×4 / meshletRemap（binding 20）×1 / `buf`×1（同上，helper 形参） |
| `rgl/shaders/cull.comp.slang` | 5 | cullTable（binding 0）×3 / sequences（binding 2）×1 / countBuf（binding 4）×1 |
| `rgl/shaders/stream_fanout.comp.slang` | 5 | streams（binding 2）×3 / cullTable（binding 0）×1 / sequences（binding 1）×1 |
| `rgl/shaders/cube.mesh.slang` | 4 | indexBuffer×3 / vertexBuffer×1 |
| `rgl/shaders/izb_create.comp.slang` | 4 | nodes（binding 1）×4 |
| `rgl/shaders/izb_mask.frag.slang` | 4 | nodes（binding 6）×3 / head（binding 5）×1 |
| `rgl/shaders/izb_prime.comp.slang` | 4 | nodes（binding 1）×2 / head（binding 0）×1 / primed（binding 3）×1 |
| `rgl/shaders/model.mesh.slang` | 3 | modelVertexBuffer×3 |
| `rgl/shaders/light.frag.slang` | 1 | izbMask（binding 14）×1 |
| `rgl/shaders/meshlet_cull.comp.slang` | 1 | clusterVisible（binding 4）×1 |
| **合计** | **49** | **`sceneData` 出现 0 次** |

**归属分组与"为什么不在本计划射程"**（四条，逐条是不同的契约归属，不是一句"遗留"）：

1. **簇化轴产物四段 buffer**（binding 17-21，`tools/meshlet_pack/format.h` 的磁盘布局）
   = 12 + 6 + 1 = **19 条**。它的契约在 `DiskCluster` 的**打包格式**上（44B 定长记录 +
   位域），不是反射可表达的 shader struct；要类型化 = 给 meshlet_pack 上反射，是
   `docs/mdi_plan.md` 的事（§6.1 已划界：簇化轴与本计划并行，冲突面只有文件归属）。
2. **gpu_cull 私有契约**（`cullTable` 60B / `sequences` 120B / `countBuf` / `streams`）
   = 5 + 5 = **10 条**。这是 `gpu_cull.cppm` **自己的**每帧中间表，从来不是 scene data
   （§9.8「末」行同判）。
3. **IZB 链表三件套**（`head` / `nodes` / `primed` / `izbMask`）= 4 + 4 + 4 + 1 = **13 条**。
   链表节点是"同一条 buffer 里混装多种记录"的形态，类型化要先把 head/nodes 拆成两张表。
4. **顶点/索引池**（`modelVertexBuffer` / `vertexBuffer` / `indexBuffer`）= 3 + 4 = **7 条**。
   它们是**逐元素类型已由 `ModelVertex` 契约钉住**的池，偏移来自 `stride × index` 的展开形态。

19 + 10 + 13 + 7 = **49** ✓ 与总数闭合，且**没有一条的接收者是 `sceneData`**
——这就是 §2「偏移写不出来了」那句话的完整证据链。

⚠ **R5 自曝盲区仍然成立**（别把这张表读成"全仓字节偏移总量"）：① `Interlocked*` 也吃字节
偏移，未纳入棘轮；② 偏移先存进变量再传参（`const uint o = 48u; buf.Load(o)`）看不见。
本表数的是「调用点内联字面量」，与基线 `total` 同口径。

### 9.11 终态读数（2026-10-13 收口，本机 AMD 8060S / RADV STRIX_HALO / Mesa 26.2.3）

**迁移完成度**：§9.5 定案表 12 个区 **12/12 已迁**（binding 22–33 全部挂上子范围描述符）。
`ByteAddressBuffer sceneData` 声明与全区描述符**已删除**；物理 buffer 仍在（各区子范围描述符
指向它的各区），`kSceneDataBytes` 787936 → **789776**（+4 个哨兵元素、−768B 阴影子区合并）。

| 读数 | 分支点 `f4ee56c` | 终态 | 说明 |
|---|---|---|---|
| R5 裸字节偏移 | 130 | **49** | 减掉的 81 条**全部**是 scene data；剩余 49 条逐条归属见 §9.10，**一条不是 scene data** |
| R6 `sceneData` 全区读 | 不存在 | **0 命中**（必须 0） | 批 N+1 新增永久护栏，两条变异（加回一条 / 规则自身失效）实测有牙 |
| `StructuredBuffer` 使用 | 0 | **4** | R3/R4 从空转变为生效 |
| 契约类型（R3 放行面） | 9 | **18** | 含 `ObjectMatrix`（批 R 登记，矩阵惯用法进契约的入口） |
| mappings（出厂默认档） | 6560 | **5697** | 批 M 过滤 −43%，之后 12 条新 binding 又加回来一部分 |
| `createTotal` | 2.98s | **2.76s** | 与 mapping 数**无关**（§9.3 已改口径：由着色器编译主导） |
| 两档像素锚点 | `54e40665…` / `95edc8cc…` | **逐位不变** | 14 个 commit、每个 commit 两档各测，无一例外 |
| `just test` / `test-gpu` | 376 例 | **384 例 / 89918 断言**、27 例 / 24722 断言 | 新增哨兵契约、嵌套反射布局等用例 |
| `just smoke --sync` | — | **hazard 0 条 / errors 0** | cull 三档（cpu / gpu_sync / gpu_lagged）各自跑过 |

**这一夜真正买到的东西**（按可复核性排序，不是按代码量）：

1. **shader 侧 scene data 零字节字面量** ⟹ §2 的成功判据成立：想再写 `sceneData.Load4(525312u)`
   会先撞上"符号不存在"，再撞上 R6 ERROR。**双源不是被"修对了"，是被写不出来了。**
2. **护栏先于迁移**（§5 顺序红线）真的兑现了：R5 棘轮从 130 一路降到 49 且**每次收账都出声**，
   R6 在收口当批就带着两条变异落地，而不是"以后再补"。
3. **三条被实测推翻的预判**都留在了文档里而不是被悄悄改掉：mapping 数 ⟹ 启动时间（§9.3）、
   GPU-AV 可用（§9.9）、"漏烘 mapping 会静默读零"（实际是 `VUID-…-11292` 启动即炸）。

**明确不做 / 射程外**（别当漏项）：`cull.comp` 私有 binding 4 挪回 3（要重跑 cull 三档冒烟）、
同构表（簇化 / cull / IZB / 顶点池）的类型化（另一条线：先给它们上反射契约）、
`uvFromWorld` 零调用者但非 scene data 读取面（§9.8 已记不删理由）、
R5 自曝盲区（`InterLocked*` 与"偏移先存变量再传参"看不见）。
