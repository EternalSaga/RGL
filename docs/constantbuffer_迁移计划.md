# scene data 类型化迁移（ByteAddressBuffer → ConstantBuffer / StructuredBuffer）

> **状态**：现行（2026-10-12 立）。本文件 = 这条线的**唯一计划载体 + 事实基线**。
> 触发源 = 簇化轴 ★1-a2-i 期间对"双源为什么必然"的架构诊断（结论见 §1）+ 用户拍板做 spike。
> spike 定案 = `tests/test_descriptor_heap_ubo.cppm`（`TEST_SUITE("gpu")`，git `308a703`）。
> 机制深版仍以各 `.cppm` / `.slang` 文件头注释为权威；本文件不重述 scene data 分区表本身
> （唯一真相源 = `rgl/scene_data.cppm`）。

## 0. 待拍板

| 编号 | 问题 | 我的建议 |
|---|---|---|
| C1 | 分支拓扑：迁移从 master 切，还是从 MDI tip 切 | 🔴 **从 MDI tip 切**（理由 §6.2，这条决定另一会话怎么开） |
| C2 | 迁移粒度：每区一 buffer，还是"标量合并一 UBO + 大表各自 SSBO" | 先做批 0 测量再定，**别先定形态再找理由** |
| C3 | `cull.comp.slang` 遗留偏移是否并入本计划 | 并入，但排最后（它是纯遗留，不阻塞任何新功能） |
| 🔴 **C4** | **R3 红线（禁 StructuredBuffer）是否按 §8.4 三条件改精确** | 未决。🔴 **未获放行前 §2 目标形态在 `rgl/` 树内不得实施**——★1-a2 已按此回退 |

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

> 🔴🔴 **本计划的整个 §2 目标形态被一条现存红线挡着，未获撤销前不得在 `rgl/` 树内实施**：
> `shader_dep_check` **R3**（`tools/check_shader_deps.py`）——「任何 .slang 出现
> StructuredBuffer = ERROR，全库零使用，**无豁免清单**」，出处 git:调试历史「灰屏」stride
> 静默错位。冲突经过、撤销条件与配套护栏见 **§8**。**这条是用户决策，不是工程默认。**

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

## 8. 🔴 未决冲突：R3 红线 vs 本计划的目标形态（**等用户拍板**）

### 8.1 红线本体

`tools/check_shader_deps.py`（构建期 `shader_dep_check`，`cmake/RglChecks.cmake` 登记）：

> **R3 任何 .slang 出现 StructuredBuffer（含 RW/Append/Consume）= 红线（全库零使用，
> 无豁免清单；git:调试历史「灰屏」stride 静默错位）**

只警告不阻断（醒目 ERROR 行、`--strict` 拿非零码）。规则自陈的理由是"stride ≥16B 对齐与
C++ sizeof 静默偏离"。

### 8.2 今天发生的事，按时间顺序（**含一个我该早点发现的疏漏**）

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

### 8.4 撤销条件（要三条同时成立，缺一条就是拿灰屏风险换整洁）

1. **R3 改精确**：从"出现 StructuredBuffer 即 ERROR"改成"**元素类型未走反射契约**才 ERROR"。
   保留牙（无契约的裸 struct 仍然红），去掉误伤（契约类型放行）。
2. **stride 实测护栏**：给 `tests/` 的 gpu 套件加**实际在用的 stride**（8 / 44 / 4，含 44 这个
   非 16 倍数的奇数 stride）的**设备端往返**用例——写已知模式到元素 1、经 shader 读回比对。
   理由：R3 的理由是一条**驱动/布局事实**，事实只能测，不能靠注释或推理定案。
3. **逐位护栏**：迁移批次逐条 `just regress`（§3.2），且**阳性对照必须真变**（§3.4 第一条：
   漏写/接错描述符是完全静默的）。

### 8.5 为什么这条**必须**用户拍板

"全库零使用、无豁免清单"是一条**意图声明**，不是一条技术结论。工程侧能证明"带反射契约的
StructuredBuffer 在本机 stride 正确"，但**推翻一条用户定下的红线不是工程权限**。
所以在 §8.4 三条落地并获放行之前，本计划的 §2 目标形态**在 `rgl/` 树内一律不得实施**。
