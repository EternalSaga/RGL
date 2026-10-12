# scene data 类型化迁移（ByteAddressBuffer → ConstantBuffer / StructuredBuffer）

> **状态**：现行（2026-10-12 立；同日 **C4 结案** = R3 红线窄化放行 + R4 stride 护栏落地，见 §8；
> 同日**开工定案** = C1/C2/C3 全部落定 + 三条新事实（子范围描述符合法 / mapping 必须按 SPV 过滤 /
> 契约矩阵禁用 `float4x4`），见 **§9 —— 开工照 §9 做**）。
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
| C3 | `cull.comp.slang` 遗留偏移是否并入本计划 | ✅ **已结案**：并入，排最后（§9.7「末」批） |
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

> 🔴 **本表已被 §9.7 取代**（批 0 的"拆 buffer 代价"经 §9.2 证明为 0，不再需要先量再定；
> 实际执行序与每批状态以 §9.7 为准）。保留本表只为留下"当初为什么这么排"的推理。

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
3. **GPU-AV 冒烟臂**：给 `just smoke` 加一臂（先例 = syncval 的 `VK_LAYER_VALIDATE_SYNC=1`
   环境变量形态）。**先按变异纪律证明它有牙**（故意越界读，必须报），再当判据用。
   它覆盖 §3.4 第二条，也是唯一能覆盖**运行期下标越界**的工具。
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

### 9.3 定案：mapping 按 **SPV 实际声明的 binding** 过滤（否则启动翻倍）

基线读数（本机 `f4ee56c`，日志 `main_impl.cppm:588`）：
`registryEntries=479 pipelinesCreated=479 handles=958 mappings=6560 createTotal=2.98s`
⟹ ≈13.7 mapping/条目。现状 `hasSceneData` 是**一个布尔烘一条 binding 3**；若改成"新
binding 全烘"，每条目 +12 条 ⟹ mapping 总数 ~1.14 万（+73%），启动时间按 mapping 面外推
接近翻倍。**所以**：`RenderPipeline` 构造期解析两份 SPV 的 `OpVariable` 装饰集，**只为
SPV 真声明了的 binding 发 mapping**；解析失败则**退回全烘 + warn**（全烘恒正确，是
fail-safe 方向——"有 mapping 未声明"空转无害已由 addSkyEntry/addLightEntry 既证）。
副产品：今天白烘的空转 mapping 也一并降掉，启动**优于**基线。
🔴 这条是**机器可验**替代手工清单（抄名单反模式）：新增 binding 不需要在 C++ 侧登记"谁用"。

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

### 9.7 分批执行序（每批一个可回退单元，子 agent 推进 + 主 agent 校验）

| 批 | 内容 | 状态 |
|---|---|---|
| **R** | 反射工具补**嵌套 struct** 支持（§9.4 推论）+ 有牙判据 | ⬜ |
| **G** | 护栏先行（§5 顺序红线）：R2 裸字节偏移棘轮 + 死 offset 常量检测 | ⬜ |
| **M** | mapping 按 SPV 声明过滤（§9.3）+ 启动面复测（必须 ≤ 基线 2.98s） | ⬜ |
| **1** | 湿面区（32B，1 消费者，无矩阵）双路打通全链路 | ⬜ |
| 2..N | 按区推进（消费者少的先）：SSR → 簇区间表 → 材质表 → MVP 表 → 阴影参数 → 辐照度 → 灯数组 → 对象矩阵 → TAA → 阴影 VP → SceneHeader | ⬜ |
| **N+1** | 删 `scene.slang` 失效 read 函数 + `scene_data.cppm` 失效 offset 常量 + `meshlet.mesh.slang` 的 `44+8/12/16/20` 字面量 + **binding 3 与 `sceneData` 声明** | ⬜ |
| 末 | `cull.comp.slang` 遗留偏移（C3，纯遗留不阻塞） | ⬜ |

🔴 **护栏（批 G）先于任何迁移批**（§5 顺序红线："先建护栏再迁移，顺序反了护栏永远补不上"）。
🔴 每批收账必须写回本节状态列 + `docs/还债实测数据.md`，**数字绑机**（AMD 8060S / RADV
STRIX_HALO / Mesa 26.2.3）。
