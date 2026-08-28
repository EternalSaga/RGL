# Descriptor Heap 纹理采样预研（Phase 3 依据）

> **状态**：调研（Phase 3 依据；**2026-08-27 三驱动交叉测试完成**：RADV / Intel ANV / NVIDIA × 5 变体（组合 / 分离+sampler heap / 分离+内嵌 sampler / 无 sampler 对照）全部像素级 PASS——descriptor heap 纹理/sampler 语义分离**三驱动均完整实现**。原 §八"RADV 只映射组合 OpTypeSampledImage"结论**被推翻**（根因 = spike shader 把 RGBA8 纹理误声明为标量 `Texture2D<float>`，Slang 语义下 R 通道 splat 到 4 通道，详见 §九）；主工程 buffer 描述符路径已三平台验证（commit `1f43d65`）。**2026-08-27 补：§十 4K 黑屏排查**——64MB 纹理 + 渲染目标 <2048 在 RADV Strix Halo 必黑，根因定位到 APU high VA 段（56 位）+ 48 位采样描述符编码不下（内核/驱动层，未解；真实纹理尺寸正常）。主工程接入实测历史见 §八（部分定性已被 §九 修正）。**2026-08-28 最终定性（§十二）：§八 全黑 / §十 4K 黑屏 / §十一 状态漂移同根 = `TexturePool` 生命周期 UAF**（e2015c6 及之前声明在 main() if 块内，块尾析构释放纹理 BO → 第 0 帧采样已释放 VA；VA 未复用=读旧页"正常"（好状态），被复用=TCP fault/全黑）——2de6731 提出 if 块即修复（commit message 误标 slot 顺序）；m3-scene 分支现场复现 + 最小修复单变量实锤（§12.6）；§十 的 high VA 定性**已改写**（master 上真实 4K 材质模型 cottage 11 张 4096² 纹理 + 目标 1280×720 <2048 两路复跑均正常零 fault → 4K 黑屏 = 同根 UAF 复用，上游 bug report 不发）；防再发教训见 §12.7（ASan 对 GPU 侧 UAF 全盲 / VVL heap 持久引用生命周期缺口待报 Khronos / 所有权规则 + texel 回读金丝雀）。

**日期**: 2026-08-25（首测 RADV）；2026-08-27（三驱动交叉复测，见 §九）
**环境**: Gentoo, Vulkan 头文件 1.4.341（系统 `/usr/include/vulkan`，主工程/spike 编译环境），loader 1.4.341（旧，扩展符号不在动态符号表）。2026-08-27 机器三卡：RADV（Ryzen 9950X iGPU，Mesa 26.2.1）/ Intel Arc A310（ANV，Mesa 26.2.1）/ NVIDIA RTX 5090（610.57.4，prime offload）。
**结论**: 双问题均有答案——
1. **Q1（关键设计约束，三驱动定案）**：RADV `imageDescriptorSize = 64B > bufferDescriptorSize = 16B`（Mesa 26.2.1；首测 32B 为旧 Mesa 值）、NVIDIA `32B > 16B`——现有 16B slot 寻址**不能直接容纳 image 描述符**；Intel `64B ≤ 64B` 恰好相等。Phase 3 slot 布局须改为统一步长 `max(bufferDescriptorSize, imageDescriptorSize)`（运行时查询，勿硬编码）；
2. **Q3/Q4 实测 PASS（三驱动，像素级验证，2026-08-27）**：同一个 resource heap 可混存 buffer（`pAddressRange` 裸地址）+ image（`pImage`/`VkImageDescriptorInfoEXT`）描述符；sampler 语义分离的两种形态——`pEmbeddedSampler`（内嵌）与独立 sampler heap（`vkWriteSamplerDescriptorsEXT`+`vkCmdBindSamplerHeapEXT`）——在组合（OpTypeSampledImage）与分离（OpTypeImage+OpTypeSampler）绑定下均工作正常。

---

## 一、背景

Phase 3（TexturePool + 模型纹理采样）要把 glTF 的 `baseColorTexture` 送进现有 descriptor heap 管线（`VmaDescriptorHeap` + `vkCmdBindResourceHeapEXT` + shader object binding mapping）。现有管线只写过 `STORAGE_BUFFER` 描述符（BDA 裸地址），从未写过 image 描述符。开写前须回答四个问题：

- **Q1**. `vkGetPhysicalDeviceDescriptorSizeEXT(SAMPLED_IMAGE)` vs `bufferDescriptorSize`——现有 slot 寻址（`slot * bufferDescriptorSize`）能否直接容纳 image 描述符？
- **Q2**. `imageDescriptorAlignment` vs 统一 slot 步长——image binding 的 heapOffset 必须按 image 描述符对齐（VUID 11251/12406），现有步长是否合法？
- **Q3**. 同一个 resource heap 混存 buffer + image 描述符是否可行（spec 允许，驱动是否认账）？
- **Q4**. `pEmbeddedSampler`（shader object 创建时烘焙 sampler）在 mesh+frag 链上是否工作（避免独立 sampler heap）？

## 二、spec 调研

spec 正文：本地 `Vulkan-Docs`（v1.4.360）`chapters/descriptorheaps.adoc`（2239 行；`appendices/VK_EXT_descriptor_heap.adoc` 是 stub）。关键条款（修订号 1.4.360，语义预计对 1.4.341 时代同样成立）：

| 条款 | 内容 | 对本项目的含义 |
|---|---|---|
| VUID 11210（`VkResourceDescriptorInfoEXT::type` 允许集） | 允许 `SAMPLED_IMAGE` / `STORAGE_IMAGE` / `UNIFORM_TEXEL_BUFFER` / `STORAGE_TEXEL_BUFFER` / `UNIFORM_BUFFER` / `STORAGE_BUFFER` 等；**不含 `COMBINED_IMAGE_SAMPLER`** | heap 采样纹理**只能** `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE`；sampler 必须走 `pEmbeddedSampler`（或独立 sampler heap）——设计定案，无选择 |
| VUID 11207 | `vkWriteResourceDescriptorsEXT` 每条目 size ≥ `vkGetPhysicalDeviceDescriptorSizeEXT(type)` | slot 步长必须 ≥ 该类型描述符实际大小 → **Q1 的规范依据** |
| VUID 11208 | ycbcr 图：size ≥ descriptorSize × `combinedImageSamplerDescriptorCount`（相邻多描述符） | 普通 sRGB/UNORM 纹理不涉及（Phase 3 用 UNORM） |
| `VkImageDescriptorInfoEXT`（:582-593） | `pView` = **`VkImageViewCreateInfo`（create info 值，非 handle）**；`layout` = 描述符访问时的 image layout | 写描述符时必须保留 image view 的 create info 结构体（不能只存 handle）；layout 用 `SHADER_READ_ONLY_OPTIMAL` |
| VUID 11458 | image 须以 `VK_IMAGE_USAGE_SAMPLED_BIT` 创建 | 纹理 image 标准 usage |
| VUID 11251 / 12406（OpTypeImage / OpTypeSampledImage 映射） | `heapOffset` / `heapArrayStride` 必须为 0 或 `imageDescriptorAlignment` 的倍数 | image binding 的 offset 对齐依据 → **Q2 的规范依据**（独立于 `bufferDescriptorAlignment`） |
| `VkDescriptorMappingSourceConstantOffsetEXT::pEmbeddedSampler`（v341 头 :15662；spec 注 :1692 附近） | "若映射资源是 OpTypeSampler 或 OpTypeSampledImage 且 pEmbeddedSampler 非 NULL，用内嵌 sampler **替代 sampler heap 查找**" | Q4 机制的规范依据——两种 SPIR-V 类型都覆盖（spike 测 OpTypeSampledImage；Slang 的 OpTypeImage+OpTypeSampler 对留主工程验证） |
| "One descriptor heap of each type can be bound"（:660+） | 一个 resource heap（`vkCmdBindResourceHeapEXT`）可含任意类型的资源描述符；sampler heap 独立（`vkCmdBindSamplerHeapEXT`） | Q3 的规范依据：buffer+image 混存合法，无需第二个 heap |
| `vkGetPhysicalDeviceDescriptorSizeEXT`（:2167-2239） | 返回指定类型描述符实际大小 N；写入只访问前 N 字节；"非 2 的幂"；**"not intended for general use，建议忽略以避免依赖不可移植 packing"** | 查询值仅作参考；稳妥做法是 slot 步长取属性上限（`imageDescriptorSize`），spike 已验证 |

### 重要发现：v341 头（本机）与 v1.4.360 spec 的 API 差异

本机头/loader 是 **v341**（主工程一直用的系统库；vcpkg 清单无 `vulkan` 依赖），比调研所读 spec（v1.4.360）**旧**。两版 descriptor heap API 表面不同——**写代码以 v341 头为准**（主工程编译环境），spike 初稿曾按 v1.4.360 写错一轮：

| 项 | v341（本机，主工程/spike 实际用） | v1.4.360（spec 新版） |
|---|---|---|
| 缓冲描述符 | `data.pAddressRange` = **`VkDeviceAddressRangeEXT`（裸 BDA 地址+范围）**；无 `VkStorageBufferDescriptorInfoEXT` | `VkStorageBufferDescriptorInfoEXT{buffer, range}`（句柄） |
| `vkWriteResourceDescriptorsEXT` | 4 参：`(device, count, pResources, pDescriptors)`，`pDescriptors` = `VkHostAddressRangeEXT[]{address, size}` | 6 参（pDescriptorData + pSizes 分开） |
| `pEmbeddedSampler` | 在 `sourceData.constantOffset` 内（附 `samplerHeapOffset/ArrayStride` 字段，vulkan_core.h:15656-15666） | `VkDescriptorSetAndBindingMappingEXT` 直接成员 |
| `VkBindHeapInfoEXT` | 单 `heapRange`（vulkan_core.h:15638-15644） | `pRanges` 数组 |
| `VkImageMemoryBarrier2` | 无 `srcScope/dstScope` | 有 |
| debug utils callback | `pMessageIdName`/`messageIdNumber`；无 `DEVICE_LOST` 消息类型 | `messageId` 等 |
| `vkGetPhysicalDeviceProcAddr` | **头与 loader 均无**（1.3.246+ 才入 core） | core |

**旧 loader 的运行时特征**（spike 实测）：`/usr/lib64/libvulkan.so` 仅导出 ~269 个符号，**不含** `vkWriteResourceDescriptorsEXT` / `vkCmdBindResourceHeapEXT` 等 descriptor heap 符号——裸 C 直调会**链接失败**。主工程无此问题：Vulkan-Hpp 的扩展函数 wrapper 走**运行时函数指针 dispatcher**（`getDispatcher()->vkCmdBindResourceHeapEXT`，`vkGetDeviceProcAddr` 解析，vulkan_raii.hpp:21808-21813）。spike（裸 C）采用同款模式：`#define GPA(name) (PFN_##name)vkGetDeviceProcAddr(dev, #name)`。physical-device 级函数（`vkGetPhysicalDeviceDescriptorSizeEXT`）GPA 解析不到（loader 返回 NULL + VVL 警告"instance level function"）——该函数 spec 本就标注 "not intended for general use"，spike 中 best-effort 跳过，属性值（`bufferDescriptorSize`/`imageDescriptorSize` 等）才是 Q1 依据。

**隐式依赖**（VVL 01387 实测）：`VK_EXT_descriptor_heap` 要求 `VK_KHR_maintenance5`（`VK_KHR_maintenance4` 一并，与主工程一致）——spike 初稿只挂 4 个扩展被 VVL 拦下。

## 三、spike（heap_texture_spike/）

独立 CMake 最小程序（模式同 `min_mesh_crash/`），**无窗口、无 SDL**：离屏 256×256 渲染 + `vkCmdCopyImageToBuffer` 回读 + CPU 像素比对（PASS/FAIL 退出码）。

- 纹理：RGBA8 两色图（左半 (0.25,0.5,0.75,1)，右半 (0.9,0.1,0.25,1)），`VK_IMAGE_USAGE_SAMPLED_BIT`
- heap：slot 步长 `max(bufferDescriptorSize, imageDescriptorSize)`；slot0 = `STORAGE_BUFFER`（`pAddressRange` 裸地址，float 0.1），slot1 = `SAMPLED_IMAGE`（`pImage`，layout `SHADER_READ_ONLY_OPTIMAL`）
- shader：GLSL mesh（全屏三角形+uv）+ frag（`uniform highp sampler2D` = OpTypeSampledImage → slot1 + `pEmbeddedSampler`（linear/clamp）；SSBO → slot0），`VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT | LINK_STAGE | NO_TASK_SHADER`
- 期望像素：左半 (0.35,0.60,0.85,1)，右半 (1.0,0.20,0.35,1)（0.9+0.1 clamp），容差 ±2/255

```bash
cmake -B heap_texture_spike/build -S heap_texture_spike -G Ninja && cmake --build heap_texture_spike/build
# RADV（本机）
cd heap_texture_spike/build && RADV_EXPERIMENTAL=heap ./heap_texture_spike
# NVIDIA（dGPU 机器，环境变量同 Justfile run-dgpu）
VK_DRIVER_FILES=$NV_ICD __NV_PRIME_RENDER_OFFLOAD=1 __NV_PRIME_RENDER_OFFLOAD_PROVIDER=3 \
  __GLX_VENDOR_LIBRARY_NAME=zink __VK_LAYER_NV_optimus=NVIDIA_only ./heap_texture_spike
```

## 四、RADV 实测结果（2026-08-25 首测；2026-08-27 复测 Mesa 26.2.1）

**heap 属性**（2026-08-27 复测值，Mesa 26.2.1；首测 `imageDescriptorSize` 为 32，Mesa 版本差异）：

| 属性 | 值 |
|---|---|
| `bufferDescriptorSize` | **16** |
| `imageDescriptorSize` | **64**（首测 32，旧 Mesa） |
| `samplerDescriptorSize` | 16 |
| `bufferDescriptorAlignment` / `imageDescriptorAlignment` / `samplerDescriptorAlignment` | 4 / 4 / 4 |
| `minResourceHeapReservedRange` | 0（RADV 保留 0 槽） |
| `minSamplerHeapReservedRange` / `...WithEmbedded` | 0 / 0 |
| `resourceHeapAlignment` | 4 |
| `maxResourceHeapSize` | 2^64-1（不限制） |
| `maxDescriptorHeapEmbeddedSamplers` | 2048 |

**问题回答**：

| 问题 | 结果 | 说明 |
|---|---|---|
| Q1 image 描述符 ≤ buffer 步长？ | **FAIL**（64 > 16） | 现有 16B slot 寻址不能容纳 SAMPLED_IMAGE 描述符。Phase 3 统一步长改 `max(buffer, image)`（运行时查询）；现有 buffer slot（0/4/8+i）**槽号不变**、仅步长变宽，每 slot 浪费可接受 |
| Q2 统一 64B 步长对齐合法？ | **PASS** | 64 % imageDescriptorAlignment(4) == 0 且 64 % bufferDescriptorAlignment(4) == 0——image 与 buffer binding 的 heapOffset 都合法 |
| Q3 单 heap 混存 buffer+image？ | **PASS** | 同一 resource heap 写两种描述符、shader object 同时引用，渲染正常（2026-08-27 三变体复测均 PASS） |
| Q4 内嵌 sampler 采样？ | **PASS** | `pEmbeddedSampler`（linear/clamp）+ SAMPLED_IMAGE heap 描述符，mesh+frag shader object 链采样正确，**4 个校验像素全部精确匹配**（(8,8)=(89,153,216,255)=(0.35,0.60,0.85,1)；(136,128)=(255,51,89,255)=(1.0,0.20,0.35,1)）；分离绑定的 sampler heap / 内嵌两形态亦 PASS（§九） |

## 五、NVIDIA / Intel 实测结果（2026-08-27 补测完成）

2026-08-27 起开发机加装 RTX 5090（NVIDIA 610.57.4，prime offload）与 Arc A310（ANV），同一 spike 全变体补测通过（完整矩阵见 §九）。**heap 属性**：

| 属性 | RADV 26.2.1 | Intel ANV 26.2.1 | NVIDIA 610.57.4 |
|---|---|---|---|
| `bufferDescriptorSize` | 16 | **64** | 16 |
| `imageDescriptorSize` | 64 | **64**（=buffer，Q1 恰好 PASS） | **32**（Q1 FAIL） |
| `samplerDescriptorSize` | 16 | 32 | 32 |
| 三对齐（buf/img/sampler） | 4/4/4 | 64/64/32 | 8/32/32 |
| `minResourceHeapReservedRange` | 0 | 0 | **96768B**（=3024 槽 @32B） |
| `minSamplerHeapReservedRange` | 0 | 0 | **512B**（=16 槽 @32B） |
| `minSamplerHeapReservedRangeWithEmbedded` | 0 | 0 | **65536B**（=2048 槽） |
| `resourceHeapAlignment` | 4 | 64 | 32 |
| `maxResourceHeapSize` | 2^64-1 | **2GB** | **32MB** |
| `maxDescriptorHeapEmbeddedSamplers` | 2048 | 2048 | 2032 |

**Q1 跨驱动定案**：RADV 64>16、NVIDIA 32>16（FAIL），Intel 64≤64（PASS）——统一步长 `max(buffer, image)` 是唯一跨驱动安全设计（运行时查询）。NVIDIA 的 `minResourceHeapReservedRange` 巨大（3024 槽）使 spike 首版暴露一个潜在 bug：`VkBindHeapInfoEXT::reservedRangeOffset` 误写成 `reservedSlots*slotSize`（堆尾，VUID 11232 越界）+ 描述符写 hostRanges 用绝对偏移 0/slot 而非 `reservedSlots+i`——RADV/Intel 保留区为 0 故从未触发；已修（reserved range 堆首 `[0, reservedSlots*slotSize)`，与主工程 VmaDescriptorHeap 同布局）。

NVIDIA 侧无 Xid 13/engine reset（spike 带 `NO_TASK_SHADER` 位），mesh shader object 路径正常。

## 六、对 Phase 3 的设计输入

1. **slot 步长**：`VmaDescriptorHeap::descriptorSize` 从 `bufferDescriptorSize` 改为 `max(bufferDescriptorSize, imageDescriptorSize)`（构造期查询）；`reservedSlots = ceil(minResourceHeapReservedRange / descriptorSize)` 随之（RADV 保留 0 无影响，NVIDIA 槽数减半、保留字节数不变）；`MeshAsset::heapSlot` 槽号语义不变，`heapOffset = slot * descriptorSize` 自动兼容。
2. **纹理描述符写入**：`vkWriteResourceDescriptorsEXT`（4 参，v341）+ `VkResourceDescriptorInfoEXT{type=SAMPLED_IMAGE, data.pImage=&VkImageDescriptorInfoEXT{pView=<image view create info>, layout=SHADER_READ_ONLY_OPTIMAL}}`；image view create info 须随 TexturePool 资产保留（不能只存 handle）。
3. **sampler**：一律 `pEmbeddedSampler`（`sourceData.constantOffset.pEmbeddedSampler`，v341 位置），不建 sampler heap；RADV `maxDescriptorHeapEmbeddedSamplers=2048` 充裕（每管线 1 个）。
4. **Slang 变体**（2026-08-27 三驱动交叉测试定案，见 §九）：分离绑定（`Texture2D<float4>`+`SamplerState`，OpTypeImage+OpTypeSampler 对）在 RADV/Intel/NVIDIA 上**全部像素级 PASS**，sampler 走独立 sampler heap 或 `pEmbeddedSampler` 均可。~~"RADV 只映射组合 OpTypeSampledImage"~~ 的旧定性（§八）**被推翻**——根因是 spike shader 把 RGBA8 纹理误声明为标量 `Texture2D<float>`（Slang 语义 Sample 返回标量，R 通道 splat 到 4 通道，设计语义非 bug）。**主工程两处必修**：`model.slang:41` 改 `Texture2D<float4>`（否则即使绑定全对，albedo 也是 (R·R, R·G, R·B) 失真）；`shader.cppm:146` 的 OpTypeSampler 映射改 `heapOffset`（`samplerHeapOffset` 字段 spec 明确"仅组合 image sampler 用"，见 §九 根因 2）。
5. **feature/扩展**：纹理路径无新增 feature；device 扩展维持现状（descriptor heap 隐式依赖 maintenance4/5 已在主工程）。

## 七、已知问题

- **VVL 09600 跟踪伪影**（RADV/Intel/NVIDIA + 系统 VVL，2026-08-27 三驱动一致出现）：spike 在 validation layer 下报
  `VUID-vkCmdDraw-None-09600 ... expects VkImage ... to be in layout TRANSFER_DST_OPTIMAL -- instead, current layout is UNDEFINED`，
  但同一运行**像素精确匹配**且关闭 VVL 后运行完全干净（exit 0，无其他 VVL 报错）——判定为 VVL 对 descriptor heap image 描述符的 layout 跟踪缺陷（写描述符发生在 host 侧、布局转换发生在命令缓冲内，VVL 关联失败），非真实 spec 违规。主工程未开 VVL 运行，不受影响。

## 八、主工程接入实测（2026-08-27 首测，时点记录）：RADV 采样恒 garbage

> **⚠ 本节"RADV 实现缺口"定性已被 §九（2026-08-27 同日三驱动交叉测试）推翻**：spike 复测证明三驱动均正确实现分离绑定，garbage 的直接根因是 shader 把 RGBA 纹理声明为标量 `Texture2D<float>`（R 通道 splat）+ OpTypeSampler 映射字段误用（`samplerHeapOffset` 应为 `heapOffset`）。本节保留为首测时的排查过程记录；"已排除"表中各环节验证仍有效，"根因"与"定性"以 §九 为准。

Phase 3 接入（`texture_pool.cppm` + `shader.cppm` 3 条目映射 + sampler heap）后，RADV 上模型纹理采样**恒返回 garbage**（`float4(1.0)+texel` 全黑 = NaN）。逐项排查后定位：

**已排除（全部验证正确）**：

| 环节 | 验证方法 | 结果 |
|---|---|---|
| GPU 纹理数据 | 临时 `vkCmdCopyImageToBuffer` 回读 texel(0,0) | ✅ 白纹理 (255,255,255,255)、真实纹理有数据 |
| 上传 layout 转换 | staging UNDEFINED→SRC / dst UNDEFINED→DST / DST→SHADER_READ_ONLY | ✅ |
| 纹理描述符布局 | v341 `vk::ImageDescriptorInfoEXT`/`ResourceDescriptorInfoEXT` hpp wrapper 与裸 struct 布局兼容（static_assert 确认），`pView`=image view create info | ✅ |
| image view | `subresourceRange` 字段序 `aspectMask,baseMipLevel,levelCount,baseArrayLayer,layerCount`，`{eColor,0,1,0,1}` 正确 | ✅ |
| slot 寻址 | CPU/GPU 均 "slot0 基址 + N×descriptorSize"，与顶点 buffer（binding0，工作正常）同机制 | ✅ |
| 几何 + binding0 映射 | fragment 改纯常量 → 模型正常绘制 | ✅ |
| host→heap barrier | **修了真 bug**：原只设 `eResourceHeapReadEXT`，漏 `eSamplerHeapReadEXT`（两 heap 各有独立 access bit）→ sampler 描述符 host write 未同步 | ✅ 已修 |
| resourceMask | 具体 mask（SAMPLED_IMAGE/SAMPLER）与 ALL_EXT 均试 | ❌ 都失败（非 mask 问题） |

**根因**：对比 spike（GLSL 组合 `sampler2D`=OpTypeSampledImage，PASS）与主工程（Slang 分离 OpTypeImage+OpTypeSampler，FAIL）——**RADV 的 descriptor heap 只映射组合 `OpTypeSampledImage`，不映射分离的 `OpTypeImage`（resource heap）+ `OpTypeSampler`（sampler heap）**。两个独立 binding 都映射不上 → 采样读未初始化内存 → garbage。

| 组合 | RADV 结果 |
|---|---|
| Slang 分离 + 独立 sampler heap（主工程现状） | ❌ 采样 garbage |
| Slang 分离 + `pEmbeddedSampler`（早先试） | ❌ 采样恒 0 |
| GLSL 组合 + `pEmbeddedSampler`（spike） | ✅ PASS |

**定性（已修正，见 §九）**：~~spec 支持分离绑定……这是 RADV 的实现缺口，非写法错误~~——§九 交叉测试证伪该定性：三驱动均正确实现分离绑定。实际根因 = ① shader 标量纹理 splat（`Texture2D<float>` 声明 RGBA8 纹理）② OpTypeSampler 映射误用 `samplerHeapOffset` 字段（spec 要求 `heapOffset`；"samplerHeapOffset 专为此设"的解读本身有误——该字段仅用于**组合** image sampler）。

**对 Phase 3 的影响（2026-08-27 更新）**：
- 纹理数据/描述符/slot/barrier 全链路在三驱动上就绪；采样绑定模型（分离 OpTypeImage+OpTypeSampler，sampler heap 或内嵌）**spike 已三驱动像素级验证**。
- 主工程落地剩两步：`model.slang` 改 `Texture2D<float4>` + `shader.cppm` 映射字段改 `heapOffset`；完成后桌面环境 `just run`/`run-igpu`/`run-dgpu` llm_debug 截图复验 albedo（本 headless 会话无显示设备，无法代跑）。
- §八 首测的"NaN 全黑"现象：splat（白纹理→白）与字段误用（RADV 保留区 0，侥幸无害）均不能解释全黑，重测后若仍异常再单独立项排查。

## 九、三驱动交叉测试（2026-08-27）与误诊复盘

### 9.1 测试矩阵（全部像素级 PASS）

机器：9950X iGPU（RADV 26.2.1）+ Arc A310（ANV 26.2.1）+ RTX 5090（NVIDIA 610.57.4，prime offload）。slangc 2026.7.1（vcpkg）。spike 新增 Slang 分离变体与无 sampler 对照变体（`SPIKE_SHADER=glsl|slang|slang_ctrl`、`SPIKE_SAMPLER=embedded|heap`、`SPIKE_SAMPLER_FIELD=heapOffset|samplerHeapOffset`；一键矩阵 `heap_texture_spike/cross_test.sh`）：

| 变体 | RADV | Intel | NVIDIA |
|---|---|---|---|
| GLSL 组合 `sampler2D`（OpTypeSampledImage）+ 内嵌 sampler | PASS | PASS | PASS |
| Slang 分离（OpTypeImage+OpTypeSampler）+ **sampler heap**（spec 字段 `heapOffset`） | PASS | PASS | PASS |
| Slang 分离 + sampler heap（旧字段 `samplerHeapOffset` 对照） | PASS | PASS | PASS |
| Slang 分离 + 内嵌 sampler | PASS | PASS | PASS |
| Slang 对照：纯 `Load`/OpImageFetch（**无 sampler 路径**）+ buffer | PASS | PASS | PASS |

**结论**：descriptor heap 的 texture/sampler 语义分离（heap 不允许 COMBINED_IMAGE_SAMPLER，image 走 SAMPLED_IMAGE 描述符、sampler 走 sampler heap 或内嵌）在**三驱动上完整实现**，组合与分离两种 SPIR-V 形态均支持。

### 9.2 误诊复盘（为什么 §八 会得出"RADV 缺口"）

首版 spike 的 Slang 分离变体在三驱动上**逐像素完全一致**地输出 `(R+0.1)×4`（R 通道广播），排查过程：

1. **决定性对照**：加"无 sampler"变体（纯 `OpImageFetch`，完全不涉及 OpTypeSampler/OpSampledImage）——**同样 (R+0.1)×4**。问题不在 sampler 分离路径，而在 image 映射或更上游；
2. **SPIR-V 反汇编定位**（spirv-dis + 二进制级核对，spirv-val 全过）：采样/取纹理结果（强制 4 分量，SPIR-V spec 规定）之后，Slang 生成了
   `OpCompositeExtract %float %sampled 0` + `OpCompositeConstruct %v4float %23 %23 %23 %23`——只抽 R 通道再广播到 4 通道。三驱动一致是必然的（shader 端代码生成，与驱动无关）；
3. **根因 1（spike 测试设计缺陷）**：spike 把 **RGBA8 纹理声明为标量 `Texture2D<float>`**。Slang（同 HLSL）语义：元素类型为标量时 `Sample`/`Load` 返回标量，SPIR-V 侧抽首分量兑现标量语义；`+ float4(...)` 触发标量→float4 隐式转换 = R 通道 splat。对照 v2026.7.1 源码（`hlsl.meta.slang:1214` 返回 T；`slang-emit-spirv.cpp:10620` 采样结果恒 4 分量向量；`:10743` 标量截断抽首分量）与官方 stdlib 文档——**设计如此，非 bug，与 Slang 版本无关**（升级到 v2026.16 行为不变）。修法：`Texture2D<float4>`。**主工程 `model.slang:41` 同款声明**——即使绑定全对，albedo 也是 (R·R, R·G, R·B) 失真，必须改；
4. **根因 2（真 spec 偏差，spike 首版 + 主工程现态同款）**：独立 OpTypeSampler 映射把 sampler heap 偏移写进了 `samplerHeapOffset` 字段。spec（`descriptorheaps.adoc` :1664-1665 + VUID 11253）：`heapOffset` 是"mapped resource **or sampler**"的堆内偏移；`samplerHeapOffset` "**used only when mapping a combined image sampler**"（:1671-1674）。正确写法：`heapOffset = samplerReservedSlots * samplerDescSize`（samplerDescriptorAlignment 倍数）、`samplerHeapOffset` 留 0。**实测对照（9.1 矩阵第 2/3 行）：三驱动对旧字段用法均宽容兼容**（NVIDIA sampler 保留区 512B 非零仍 PASS）——故旧字段不是 §八 garbage 的当前直接原因，但 spec 层面是 UB，必须修；
5. **顺带澄清（排查中一度怀疑的方向）**：`OpTypeImage %float 2D X 0 0 1 Unknown` 的 X 是 **Depth** 字段（操作数序：Result, Sampled Type, **Dim**, **Depth**, Arrayed, MS, Sampled, Format——Depth 在 Dim 之后、Arrayed 之前）。GLSL `sampler2D` 断言非深度（X=0）；Slang 格式无关类型用 X=2"无指示"，spec 三值均合法，不影响采样语义，spirv-val 放行两者正确。

### 9.3 跨驱动 heap 属性（Q1/Q2 设计输入）

见 §五 三列表。要点：`imageDescriptorSize`（RADV 64 / NVIDIA 32 / Intel 64）> `bufferDescriptorSize`（16/16/64，Intel 除外恰好相等）→ **统一步长 `max(buffer, image)` 运行时查询**；NVIDIA resource 保留区 96768B（3024 槽 @32B）+ sampler 保留区 512B（16 槽 @32B）→ 用户槽必须从 `reservedSlots` 起（spike 首版在此越界，已修，见 §五）。

### 9.4 Slang 版本管理（2026-08-27 已落地：FetchContent 替代 vcpkg）

- vcpkg `shader-slang` port 停在 **2026.7.1**（上游 master 亦如此；Slang 月更，落后 9+ release）——`vcpkg update` 不可用；
- **现方案（2026-08-27 实施）**：`cmake/SlangFetch.cmake` 的 `rgl_fetch_slang()`——FetchContent pin **v2026.16**（2026-08-20，官方首个 Linux 预构建 release）+ sha256 校验，解压到 `build/_deps/slang-prebuilt-src/bin/slangc`。根 CMakeLists 顶部无条件调用（主工程）；spike 独立工程同文件复用（`$ENV{SLANGC_DIR}` 可显式覆盖，**刻意不查系统 PATH**——vcpkg 旧 2026.7.1 残留在 PATH 会被误拾，首接即踩中）；`vcpkg.json` 已移除 `shader-slang` 依赖（manifest sync 自动卸载）；
- 升级：改 `SLANG_VERSION` + 从 release 页取资产 sha256 回填（资产命名 `slang-<ver>-linux-x86_64.tar.gz`，79MB；glibc 老系统换 `-glibc-2.27` 变体）；
- **2026.16 验证**：主工程全量重编（反射 JSON ×2 + spv ×4 + 生成 cppm ×3，schema 校验 PASS，契约常量不变）；spike 三驱动 × 5 变体矩阵复跑 **15/15 PASS**（与 2026.7.1 行为等价，含 SPIR-V 采样形态——2026.16 仍对 `Texture2D<float4>` 发标量采样类型 + 4 分量结果，无 extract，与 9.2 验证的 PASS 形态一致）。v2026.11–16 的 descriptor heap 修复全在 `DescriptorHandle`/`ConstantBuffer` bindless 路径（未使用）；标量纹理 splat 是语义非版本问题（9.2）。

---

## 十、4K 黑屏排查（2026-08-27）：APU high VA 段 + 48 位采样描述符（根因定位到内核/驱动层，未解）

### 10.1 现象与复现

**触发条件（三者同时）**：RADV（Strix Halo APU 核显，Mesa 26.2.1）+ **64MB（4096×4096）sRGB 纹理** + **渲染目标 <2048**（1280×720 / 1920×1080 均必黑；2048×2048 / 4096×4096 均正常）。≤16MB（2048×2048 及以下）纹理**任意目标尺寸均正常**。交互模式下 spectacle 截屏抓到 **GPUVM TCP 权限 fault**（fault 地址 `0x80010590b000` = high VA `0xFFFF80_010590_b000` 的低 48 位）。

**复现**：grass 模型内嵌纹理实际仅 **256×512**（正常渲染）；用 `TEXTURE_FORCE_SIZE=4096`（texture_pool.cppm 的 DIAG env，近邻上采样到 4096×4096）制造 64MB 纹理即复现。`RGL_NO_VVL=1 RADV_EXPERIMENTAL=heap TEXTURE_FORCE_SIZE=4096` + llm_debug 截图（非黑百分比判据）。

### 10.2 根因链（lldb 实锤，debug 驱动 -O0 -g mesa 构建 + ICD json 切换）

1. **APU 56 位 VA 空间**：设备属性 `high_va_offset = 0xffff800000000000`、`address32_hi = 0xffff8000`（`ws->info.*`，内核 AMDGPU_INFO ioctl 上报；内核 7.1.10 源码中 `high_va` 仅 UAPI 字段、驱动未引用——段选择在内核 gpuva allocator）。
2. **主工程 64MB 纹理 BO 落 high VA 段**：`iview.image->bindings[0].addr = 0xffff800100200000`（56 位，高字节 0xFF）；**spike 同尺寸 64MB 纹理落低 VA**（描述符解码 `0xC20109200000`，48 位内）→ 全配置 PASS。**这是 spike PASS / 主工程黑 的关键差异**。
3. **48 位采样描述符编码不下 high VA 地址**：GFX 纹理描述符只编码 48 位（`desc[0]` = `va>>8` 低 24 位 + swizzle，`desc[1]` `BASE_ADDRESS_HI` = `va>>40` 高 8 位，`ac_descriptors.c ac_set_mutable_tex_desc_fields`）；主工程 4K 描述符实测 `[01002000 c4200080 03ffc3ff 91b00fac | 00000000 00400000 00000000 00000000]`（地址部分 = shadow 基址 `0xC4_0000_0000` + 偏移，DCC meta 全 0 = 无 DCC）；GPU 按截断/shadow 地址读取 → 读到未映射内存 → 黑屏 / TCP fault。
4. **shadow 映射有效性状态相关**：同纹理同 BO 偏移，渲染目标 ≥2048 时采样正常（shadow 映射有效），<2048 时失效。与目标尺寸的相关性**尚未解释**（候选：目标尺寸改变 GPU 页表/TLB 配置或 VA 布局状态）——需内核级调试（dmesg / amdgpu debugfs，须 root）。

### 10.3 已排除项（用户态二分，2026-08-27）

| 假设 | 实验 | 结论 |
|---|---|---|
| VVL 双写污染描述符 | `RGL_NO_VVL=1`（vkinstance.cppm 临时 env 门）关 VVL（/proc maps 0 映射）仍全黑；WP run 里 VVL wrapper 的"零写"实为 `pImage=NULL` 路径（`radv_descriptors.c` 外层 `else memset` 分支）+ lldb 初始状态报告伪影 | 排除 |
| 分配时序（早分配占低 VA） | main 把 64MB 纹理**预分配到 swapchain 前**（raw Vulkan 独占，同 spike 方式）+ TexturePool 按 w/h 采纳 → BO 地址**仍 0xffff80...**（high VA 段内更早偏移 0x0100200000），仍黑 | 排除（**改动已回退**——无效） |
| VMA 子分配 | `VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT` 强制独占 → 仍黑 | 排除（DIAG 保留） |
| 内存池选择 | 4K 与 1×1 白纹理 VMA 均 type=0 pool=0 `DEVICE_LOCAL`（DIAG 日志），与 spike 同池 | 排除 |
| 描述符 heap 容量 | spike `SPIKE_HEAP_SLOTS=1026`（=主工程 capacity）膨胀后仍 PASS；spike heap gpuAddr 同为 high VA（0xffff8000000c0000）但经 64 位 push constant 传递不受 48 位限制 | 排除 |
| shader 变体 | spike 4K + 1280×720：GLSL 组合 / Slang+heap / Slang+embedded 三变体全 PASS | 排除 |
| device swapchain 扩展 | spike `SPIKE_SWAPCHAIN_EXT=1` 追加 VK_KHR_swapchain（不建对象）仍 PASS | 排除（嫌疑收窄到主工程 features 组合 / VMA / 窗口+surface 交互） |

**spike 侧重要勘误**：spike `renderArea` 此前误用 `W×H`（纹理尺寸）而非 RT 实际尺寸（`SPIKE_RT_*` 覆盖时 renderArea 越界 = UB）——"4K+小 RT PASS" 早期结论被污染，修复后（`heap_texture_spike/main.cpp` renderArea = RTW×RTH）**复测仍 PASS**（4K + 1280×720 三变体）→ spike 与主工程的差异确实在 app 结构（device 配置 / VMA / 窗口），不在渲染用法。

### 10.4 实测矩阵（主工程，-O0 debug 驱动，4K 纹理；BO 偏移均为 high VA 段）

| 渲染目标（离屏，llm_debug） | 纹理 | 结果 |
|---|---|---|
| 1280×720 | 4096（64MB） | ✗ 0% 全黑 |
| 1920×1080 | 4096（64MB） | ✗ 全黑 |
| 2048×2048 | 4096（64MB） | ✓ 70.8% |
| 4096×4096 | 4096（64MB） | ✓ 45.9% |
| 1280×720 | 2048（16MB） | ✓ 84.4% |
| 1280×720 | 1024 | ✓ 83.3% |
| 1280×720 | 256×512（真实） | ✓ 正常 |

**KWin 干扰（已解耦）**：隐藏窗 swapchain extent 协商**不稳定**（同配置运行间漂移：有时 clamp 到 1920×1080，1:1 尺寸有时照给）——`llm_debug.cppm` 目标尺寸改用**配置的窗口尺寸**（`cfg.width/height`）而非 swapchain extent，截图尺寸确定；分离实验（LLM_TARGET env，已回退）证明**离屏渲染目标尺寸是判别变量、swapchain extent 不是**（窗口 4096 + 目标 1280 黑；窗口 1280 + 目标 4096 正常）。

### 10.5 lldb 方法（可复用）

- **debug 驱动**：`/tmp/opencode/mesa-build/src/amd/vulkan/libvulkan_radeon.so`（mesa 26.2.1，-O0 -g 带符号）；ICD json `/tmp/opencode/radv_debug_icd.json`（`VK_ICD_FILENAMES=` 加载，不动系统驱动）。meson：`-Dbuildtype=debug -Dvulkan-drivers=amd -Dgallium-drivers=softpipe -Dopengl=false -Dgles1=false -Dgles2=false -Dglx=disabled -Dvalgrind=disabled -Dvulkan-icd-dir=/tmp/opencode/vulkan-icd` + `ninja src/amd/vulkan/libvulkan_radeon.so`。
- **断点**：`breakpoint set -f radv_descriptors.c -l 233`（`radv_WriteResourceDescriptorsEXT` 内 image 描述符写调用点；文件名不带路径；`breakpoint add name` 对 local 符号 batch 解析不了）；命中序：白纹理 → grass（`run` → `continue`×1）。
- **存址技巧**：`*(unsigned long long*)&active_trays = addr`（lldb 全局变量存任意地址）；dump 必须**解引用** `((unsigned*)(*(unsigned long long*)&active_trays))[i]`（漏解引用 = 读全局变量自身，整组数据作废）；`finish` 跑完 write 函数再 dump 全量 32B。
- **结构体路径**：`iview.image->bindings[0].addr`（BO VA）；`iview.image->vk.base.device` → `radv_device`（vk_device 首成员）→ `->ws` → `radv_amdgpu_winsys`（base 首成员）→ `->info.address32_hi / high_va_offset`。
- **WP 伪影**：8B watchpoint 的 "hit: new value: 0x0"（无 old value、进程仍停着时）= lldb 初始状态报告，不是真写入。

### 10.6 环境异常记录（Gentoo v341 头）

系统 `/usr/include/vulkan/vulkan_core.h`（v341）**无 `vkFreeDeviceMemory`**，对应函数名为 **`vkFreeMemory`**（非标准命名，官方头为 `vkFreeDeviceMemory`）；hpp `vk::freeMemory` 跟随。裸 C 释放设备内存须用 `vkFreeMemory`（本环境）。另 `vk::raii::Device` 只有 `operator vk::Device()`（不直接转 `VkDevice`）——裸 C 调用须 `*raiiDev`（经 `operator*` 转 hpp Device 再隐式转）。

### 10.7 现状与后续

> **12.6 勘误（已定性，2026-08-28 傍晚遗留验证 PASS）**：本节 lldb 观测（4K BO 落 high VA 段、
> "48 位描述符编码不下"）全部在 **UAF 修复前**构建上取得——纹理 BO 第 0 帧前已被释放，
> "high VA BO" 实锤为**已释放 VA 被复用后的分配**，非原始纹理。验证（12.6 末）：master（UAF 已修）
> 上真实 4K 材质模型（cottage，11 张 4096² 64MB 内嵌纹理）在 §10.4 的精确配置（目标 1280×720
> <2048）+ 可见窗两路复跑**均正常零 fault** → 本节"内核/驱动层 48 位编码"定性整体改写为
> **UAF 复用**，①②③ 后续（root 内核调试 / 上游 bug report / spike 二分）**全部撤回**。

- **真实纹理尺寸（256×512 级）渲染正常，Phase 3 M3 功能可用**；64MB+ 大纹理 + 目标 <2048 记为**已知驱动限制**（定性待 12.6 遗留验证改写）。
- 无效改动已回退（预分配链：`preallocateTextureImage`/`probeImageSize`/`PreallocatedTexture`/TexturePool 采纳/main.cpp 重排/GltfImage 宽高/LLM_TARGET env）；保留的诊断设施：`TEXTURE_FORCE_*` env + DIAG 日志（texture_pool.cppm）、`RGL_NO_VVL` 门（vkinstance.cppm）、spike `SPIKE_RT_*`/`SPIKE_HEAP_SLOTS`/`SPIKE_SWAPCHAIN_EXT` env + renderArea 修复。
- **后续**（未做）：① root 内核级调试（黑屏 run 时 `sudo dmesg -w` 抓 amdgpu VM fault / 页表错误，确认 shadow 映射缺口；内核源码 `/usr/src/linux-7.1.10-lto-sound-fix`）；② 上游 bug report（Mesa RADV + 内核 amdgpu：APU high VA BO 的 48 位采样描述符编码，附 §10.4 矩阵 + §10.2 地址证据）；③ 嫌疑收窄二分（主工程 device features 逐项加到 spike：timelineSemaphore / descriptorIndexing / vulkan11-14 / shaderUntypedPointers / VMA / 窗口+surface）。

## 十一、2026-08-28 可见窗 GPUVM fault 排查（交互模式 device lost；状态漂移 + per-process 触发上下文；**2026-08-28 下午定性 = TexturePool UAF，见 §12.6**）

> **12.6 勘误（本节观测全部重新归因）**：本节排查时的构建为 2de6731 开发中的**修复前**形态
> （TexturePool 仍在 if 块内，生命周期 bug 未修）。"状态漂移/per-process/分钟级翻转"=
> **UAF 的非确定性**（已释放纹理 VA 的复用模式逐运行不同）；fault 地址签名（§11.1
> fa000/fb000/e4000/...）与 12.6 在 m3-scene 分支（e2015c6 原样）上的复现 fault
> （0x8001000fa000 / 0x8001000fb000）**同一模式**，实锤同因。§11.5 group 数假设随之失去
> 意义（group 数不是触发变量——m3-scene 复现的 fault 与 group 数无关，仅与生命周期有关）。
> 本节其余内容（现象/签名/判别矩阵/排查坑）作为时点记录保留。

### 11.1 现象与 fault 签名（dmesg 无 root 可读）

交互模式（非 llm_debug）`just run` 在 `vkQueueSubmit` 崩 `VK_ERROR_DEVICE_LOST`；dmesg 每次 fault 后
"device wedged, but no recovery needed" + "Ring reset succeeded"。fault 签名（多地址变体实测：
fa000/fb000/e4000/e5000/10a000/10b000/114000/115000，同模式）：

- 地址 `0x8001_000f_xxxx`（48 位）= **2^47 + 4GB + ~1MB**——VRAM aperture（`[2^47, 2^47+VRAM)`，
  本机 boot 日志 `VRAM: 512M 0x800000000000-0x801FFFFFFF`（BIOS AUTO）之后的**高区 PTE 映射地址**
- `PERMISSION_FAULTS=3, RW=0, WALKER_ERROR=0, MAPPING_ERROR=0`，client=**TCP**（纹理管线），本进程 vmid/pasid
- fault 时机 = **frame 2**（第 3 个渲染帧，启动后 ~2s；Renderer 帧计数实测，非帧数累积）

### 11.2 §十 的"48 位截断"理论修正（重要）

- `amdgpu.vm_size=-1`（auto），内核 boot 日志 `vm size is 262144 GB` = **2^48**——GPU VA **实际就是 48 位**
- §10.2 的 `high_va_offset = 0xffff800000000000` 只是固件上报的**能力值**，非激活配置；"56 位 VA"定性作废
- fault 地址是 **48 位 VA 内的合法地址**（aperture 后高区）——**不是截断**；PERMISSION_FAULTS=3 =
  **PTE 存在但权限错** → 根因层 = 内核 amdgpu 页表/VM 状态管理（shadow 映射状态 bug），非描述符编码
- 支持"descriptor heap 机制无设计缺陷"的质疑：spike（同 APU 同 RADV，SAMPLED_IMAGE + 内嵌/独立
  sampler，**4096×4096 64MB 纹理**）像素级 PASS——机制与尺寸本身都正常，问题在 app 上下文

### 11.3 状态漂移 + per-process（不是全局污染）

- **状态漂移**：坏/好状态分钟级翻转（同配置背靠背运行结果翻转；llm 隐藏窗 16/18 → 0/12 连续）。
  坏状态下模型路径 **18/18 连续 FAIL（frame 2）**，持续 ~45 分钟；之后翻回好状态（7/7 PASS，交互可见窗正常）
- **per-process**：坏状态时主工程 12/12 FAIL，**spike 同时刻 5/5 PASS**（再跑仍 PASS）→ 触发在
  app 自身上下文（窗口/swapchain/VMA/设备配置），非全局 GPU 状态污染（ring reset 不传染 spike）
- 复现判据：`timeout 25 just run 2>&1 | grep -E "GPUVM fault|device lost"` + `dmesg | grep "page starting at address"`

### 11.4 判别矩阵（坏状态实测；PASS/FAIL = 是否 frame 2 fault）

| 上下文（swapchain 均存在） | 纹理采样 | 结果 |
|---|---|---|
| cube only（窗口+swapchain+present+VMA+全部 device 特性） | ✗ | **PASS** 3/3 |
| cube + 1×1 白纹理（`RGL_CUBE_TEX`，4 绑定管线） | ✓ | **PASS** 3/3 |
| cube + 256×256 白（`TEXTURE_FORCE_WHITE=1 TEXTURE_FORCE_SIZE=256`） | ✓ | **PASS** 3/3 |
| 模型 transparent.glb（BDA + mesh task + 1×1 白） | ✓ | **FAIL** 3/3 |
| 模型 grass（BDA + mesh task + 256×512 真实） | ✓ | **FAIL** 12/12+ |
| 模型 grass + `RGL_NO_PRESENT=1`（不 present，KWin 不 import dmabuf） | ✓ | **FAIL** 3/3 |
| 模型 grass 隐藏窗（`RGL_SHOW_AFTER=999999`） | ✓ | **FAIL** 12/12 |
| spike（mesh shader + 纹理采样，**无窗口/swapchain**） | ✓ | **PASS** 6/6（坏状态同时刻） |
| `RGL_OFFSCREEN=1`（离屏渲 + blit 上屏） | ✓ | **FAIL** 3/3（workaround 无效） |

**排除项**：present/compositor dmabuf import（no-present 仍 fault）、窗口可见性（隐藏窗 fault）、
VMA（cube 路径全 VMA 仍 PASS）、device 特性/扩展（cube 路径全特性仍 PASS）、BIOS 显存
（48GB 硬分配→AUTO 512MB，fault 地址模式完全相同）、VVL/HDR/窗口尺寸（§十 已排除项复验）。

**结论**：触发 = **模型路径（BDA 缓冲 + mesh task）+ 纹理采样 + swapchain（dmabuf BO）共存**；
"dmabuf + 任意纹理采样"单独不足（cube 全 PASS）。

### 11.5 头号嫌疑：mesh task group 数（未验证）

grass 模型 1554 三角形 ÷ `kTrianglesPerGroup=10` ≈ **155 groups**（4 个 draw 分摊）；
cube/spike = **1 group**。坏状态下 cube（1 group）+ 纹理 PASS、模型（~155 groups）+ 纹理 FAIL。
**注意**：2026-08-28 的"g128 PASS"是**假阳性**（config 编辑静默失败，实际跑的 model="" cube-only）——
kTrianglesPerGroup 10→128（≈13 groups）**尚未在真模型上验证**。下次坏状态复发时的首选实验：
`model_schema.lua` constants + `model.mesh.slang` 字面量/容量数组同步改 128（384 verts < RDNA3 512 上限），
真模型（grass）×5 判帧 2 fault 是否消失。

### 11.6 排查坑与勘误（今日踩的）

- **slot 契约勘误**：实际布局 = cube 顶点 **0** / 索引 **1**（`HeapSlotVertexOffset/HeapSlotIndexOffset`）、
  模型 base **2**（`HeapSlotModelVertexBase`，schema 生成）——AGENTS.md 的"cube 0/4、模型 base 8"
  **已过时**。`addCubeTexPipeline` 硬编码 0/4 → 索引描述符悬空 → mesh shader 读垃圾索引**静默不发射
  几何**（无 VVL 报错、截图全背景色）——binding mapping 错 slot 是静默故障，排查时先打 slot 日志
- **TexturePool 提前上传白纹理 = slot 漂移**：白纹理抢 slot 2（模型 base 2 之前）→
  `model.vertex[0]` drift 抛错。布局约定"纹理首槽 = 白"依赖**模型顶点槽先分配完**（main.cpp 已修复：
  池本体外提、`getWhite()` 留在模型块内 upload 之后；无模型时 RGL_CUBE_TEX 门补取）
- **llm `delay_frames=0` 的 PASS 假阳性**：首帧截图后立即退出，**够不到 frame 2 fault 窗口**——
  判坏状态必须用交互运行（或 delay_frames ≥ 30）
- **RAII ImageView 默认构造已删 + 构造期即调 vkCreateImageView**：null image 不能进初始化列表
  （校验拦截 abort）；需惰性时用 `std::optional<raii::ImageView>` + emplace（vkrenderer RGL_OFFSCREEN 路径）
- **新 shader 文件须同时改两处 CMake**：`compile_slang()` 只注册 custom command，
  SPV 必须**列进 add_executable 源列表**才触发构建（漏 = 运行时 "failed to open file: *.spv"）
- devcoredump（`/sys/class/drm/card0/device/devcoredump/data`）0 字节且 root 权限——无 VM 状态数据；
  内核源码只有 Kconfig/Makefile 桩——gpuva allocator 段选择逻辑读不到

### 11.7 现状与后续

- **好状态下交互显示正常**（用户在场确认，草模型 + 纹理采样；贴图"黑条纹" = alphaMode 未实现的
  Phase 5 限制，非渲染错误；glb 实测 4 primitive 全用近全 UV，无 atlas 分区）；**坏状态 frame 2 崩**
- 实用 workaround = **重试**（状态分钟级翻转，多试几次进好状态）；`RGL_OFFSCREEN`/离屏路径无效
- 诊断设施（全默认关）：`RGL_CUBE_TEX`（cube_debug.frag.slang + RenderPipeline 4 条目形态
  texturedKeepIndex + addCubeTexPipeline）、`RGL_OFFSCREEN`（离屏+blit，证无效）、`RGL_NO_PRESENT`、
  `RGL_SHOW_AFTER`（隐藏启动+N 帧后显示）、`RGL_WIN_SIZE`（交互窗尺寸）、`RGL_NO_HDR`、
  Renderer 帧计数（device lost 打帧号）
- **后续（12.6 后更新）**：~~① group 数实验（10→128）~~ 失去意义（§11 勘误注：group 数非触发
  变量）；~~② 上游 bug report（frame 2 fault 方向）~~ 撤回（根因 = 用户态 UAF，非内核）——
  改由 §12.6 遗留验证决定：master 上 `TEXTURE_FORCE_SIZE=4096` 复跑，若 4K 仍 fault 才重新
  评估内核方向（届时 §十 的 lldb 证据需按 UAF 修复后构建重取）；③ 若 4K 复跑正常 →
  §十 整体改写为 UAF 复用，上游报告不发

## 十二、2026-08-28 A/B 上屏验证 + 现场复现：全黑/4K 黑屏/状态漂移同根 = TexturePool 生命周期 UAF（2de6731 修复，commit message 标错为 slot 顺序）

> **本节最终结论（12.6 现场实验后修正 12.4 的 barrier 定性）**：§八"全黑 = NaN"、§十"4K 黑屏 /
> GPUVM TCP fault"、§十一"状态漂移（好/坏状态）"是**同一个 bug 的三种表现**——
> **e2015c6 及之前（7e1abce/afe2ff0 同款）的 `TexturePool` 声明在 `main()` 的 `if (modelPath)` 块内
> （main.cpp:104），块尾（:155）析构 → 纹理 BO 内存 + 描述符指针链存储在第 0 帧渲染前全部释放**；
> pipeline mapping 已烘焙的 heap slot 描述符仍引用已释放 BO 的 GPU VA → 第 0 帧起 GPU 采样 =
> **use-after-free**。VA 未被复用 → 读旧页数据 = 渲染"正常"（= §十一"好状态"，16/18 通过就是它在
> 正常读旧数据）；VA 被复用为权限不同的 BO → **GPUVM TCP 权限 fault**（可见窗，swapchain dmabuf
> 被 KWin import 后复用模式冲突）/ **全黑**（llm 隐藏窗）。**2de6731 把 `TexturePool` 声明提出
> if 块（生命周期覆盖渲染期）即此修复**——commit message 只记录了 slot 顺序动机（getWhite 保持块内），
> 生命周期效果未记录，导致后续排查（含本节 12.1–12.4）一直以为"已修复"而找不到根因。

### 12.1 实验矩阵（交互可见窗 800×600，用户在场目视确认；A–G 在 master 上做，master 已含 UAF 修复，故均复现不出原始症状）

| # | 变体（单变量，其余保持当前代码） | 结果 |
|---|---|---|
| A | 当前代码（deque 稳定存储 + dedicated 位 + texel 回读） | ✓ 正常（草 + 贴图 + 已知 alpha 黑条纹，§11.7） |
| B | 去 `VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT`（回 VMA 池子子分配） | ✓ 正常 |
| C | 描述符结构体回退栈局部（viewCreate/imageDesc/desc/dst）+ sampler 回退栈上 array（=e2015c6 前存储方式） | ✓ 正常 |
| D | C + 删 texel 回读块 + 删 `eTransferSrc`（upload 路径 ≈ e2015c6 前原样） | ✓ 正常 |
| E | D + **shader 回退 afe2ff0 前**（`Texture2D<float>` 标量 + `samplerHeapOffset` 旧字段） | **几何 + 采样全正常，仅颜色变黑白灰（R 通道 splat）** |
| F | 当前代码 + **Slang 回退 2026.7.1**（vcpkg 残留包，`SLANGC_DIR` 覆盖重编全部 SPV/反射） | ✓ 正常（版本无关，§9.4"行为等价"记录再确认） |
| G | 当前代码 + **host→heap barrier 回退 e96f789 形态**（缺 `eSamplerHeapReadEXT`） | ✓ 正常（当前启动期 fence/waitIdle 同步掩盖缺失，spec 层面仍必须修） |

**结论**：
- A–G 中**唯一产生可见差异的回退 = E 的标量 shader**（R 通道 splat 灰化）——贴图正常显示的可见修复 = **afe2ff0 的 `Texture2D<float>` → `<float4>`**；
- e2015c6 的 upload 路径改动（deque / dedicated / 回读 / eTransferSrc）在本机 RADV 26.2.1 上**均无行为差异**（B/C/D 逐个回退贴图照常）；
- Slang 版本（F）与 barrier 缺位（G）在当前代码状态下均无可见影响。

### 12.2 "采样期解引用描述符指针链" 归因证伪（e2015c6 commit 消息 + texture_pool/vkresources 旧注释）

Mesa 26.2.1 源码（portage distfiles `mesa-26.2.1.tar.xz`，与系统 `/usr/lib64/libvulkan_radeon.so` 同源）逐行核实：

- `radv_WriteResourceDescriptorsEXT`（radv_descriptors.c:196-241）：**写时**即 `radv_image_view_init(&iview, device, desc_info->pView, {.from_client=true})` 解析 pView → `radv_image_view_init` 内**立即** `radv_image_view_make_descriptor`（radv_image_view.c:792-798）从 `iview->image->bindings[].addr`（BO VA）构建 64B GFX 纹理描述符 → `radv_write_image_descriptor`（radv_descriptors.h:122-155）**memcpy** 进 heap slot → `radv_image_view_finish` 销毁临时 iview；
- 全树搜索：`desc_info->pView` 仅 :220/:229 两处解引用，**均在写调用内部**，写后不保留任何客户端指针（`pResources`/`pImage`/`pView`/`pDescriptors`/`pSamplers` 皆然）；
- sampler 路径（:176-194）：`radv_sampler_init` → `memcpy(sampler->state)` 写时完成；
- `vkCmdBindResourceHeapEXT`（radv_cmd_buffer.c:17001-17037）只记 heap 地址 + dirty 标记，描述符实际拷贝在 pipeline bind 期**从 heap 内存**（自包含内容）进行；
- spec v341 validusage.json：`vkWriteResourceDescriptorsEXT` 指针仅要求**调用时**有效，无"保持至描述符销毁"条款。

即：本驱动写路径**即时解析**，heap slot 收到自包含的最终描述符；C/D 实验（栈局部结构体照常工作）与源码一致。deque 稳定存储的正确定位 = **跨驱动防御性卫生**（spec 允许驱动延迟解释 create info 语义，其他驱动/未来 RADV 可能真持有指针），非本机因果。

### 12.3 排除的文档/commit 猜想

| 猜想 | 出处 | 处置 |
|---|---|---|
| "RADV 采样期解引用描述符指针链，4 个栈临时对象悬空 = 全黑根因" | e2015c6 commit 消息 + texture_pool.cppm/vkresources.cppm 旧注释 | **证伪**（§12.2 源码 + B/C/D 实验）；旧注释已改指本节 |
| "4K → GPUVM TCP fault = 悬空栈槽复用 → 坏地址" | e2015c6 commit 消息（4K 部分） | **证伪**（§11.2 已修正为 PTE 权限状态 bug；D 再排除：256 + 栈局部 + 无 dedicated 无 fault；4K 问题 = 尺寸/VA 段相关独立内核层问题，未解） |
| "旧字段 `samplerHeapOffset` 用法 → 采样 garbage" | §八 根因 2 / §九.2 | **功能层面排除**（E 用旧字段采样正常；与 §9.1 三驱动宽容兼容一致）——`heapOffset` 修正定性为 spec-UB 修正（必须修，但非功能修复） |
| "RADV 只映射组合 OpTypeSampledImage，不映射分离对" | §八（§九 已推翻） | **再确认排除**（E：分离 OpTypeImage+OpTypeSampler + sampler heap 全链路正常） |

### 12.4 §八"全黑 = NaN"的定性修正（12.6 现场实验后）

- **根因 = UAF（12.6）**：§八观察的"采样恒 garbage / 全黑 = NaN"（08-27，M3 首接期）发生在
  7e1abce/afe2ff0/e2015c6 代码下——`TexturePool` 全部在 if 块内（三 commit 的 main.cpp 均为
  :104 块内声明），第 0 帧采样已释放的纹理 BO。garbage/NaN = 读已释放 VA 的旧页/复用页；
- **barrier 缺位是次要的、已被掩盖的 spec 违规**（本节初版曾把它定性为主根因，修正）：
  `e96f789`（7e1abce 前）heap barrier 缺 `eSamplerHeapReadEXT` 属实（sampler 描述符 host 写
  未同步 → 理论上采样未初始化内存），**7e1abce 内已修**；但 G 实验证明当前启动期
  fence/waitIdle 已掩盖该缺位的影响（回退后仍正常）——它贡献过早期"garbage"症状的一层，
  但单独不足以解释全黑/NaN 的量级（未初始化内存 ≠ 恒 NaN）；spec 层面该位仍必须保留；
- §九 的"(R+0.1)×4 splat"（三驱动 spike 交叉测试）与 UAF **无关**——spike 的纹理存储是
  `main()` 作用域（永活，无 UAF），splat 是纯标量声明问题（afe2ff0 修，E 实验实锤灰化）；
- 三个症状按修复 commit 分层：barrier → 7e1abce 修（次要）、标量 splat → afe2ff0 修（灰化）、
  **UAF → 2de6731 修（全黑/4K/状态漂移的主根因）**；§八 当时把叠加症状误归因"分离绑定不被映射"，
  §九 修正一半，12.6 补全；
- F 实验补充：`cmake/SlangFetch.cmake` 新增 `$ENV{SLANGC_DIR}` 显式覆盖（同 spike 模式，刻意不查 PATH）——
  试别版本/离线用，2026.7.1 vcpkg 残留包位于
  `~/program/vcpkg/packages/shader-slang_x64-linux-native/tools/shader-slang/slangc`；

### 12.6 现场复现 + 最小修复实验（m3-scene 分支，2026-08-28 下午；决定性证据）

`git switch -c m3-scene e2015c6`（UAF 修复前的"现场"）：

| # | 变体（m3-scene 上单变量） | 结果（用户目视 + 日志判据） |
|---|---|---|
| H | e2015c6 原样 + llm_debug 隐藏窗 2048×2048（该 commit 自带配置） | **截图 100% 全黑**（中心区 mean=0 std=0），无 fault，正常退出 |
| I | e2015c6 原样 + 可见窗（llm_debug=false） | **确定性 GPUVM TCP 权限 fault**（0x8001000fa000 / 复跑 0x8001000fb000 相邻页，**首个 submit** device lost）——2/2 复现 |
| J | I + **仅 `TexturePool` 声明从 if 块内（:104）提到块外**（getWhite 保持块内，slot 顺序契约不变，其余零改动） | **3 轮可见窗全正常**（贴图在，0 fault） |

**J 单变量实锤**：生命周期 = 全黑/4K 黑屏/状态漂移的根因；I 的 fault 地址模式（相邻 4KB 页、
GPU VA 高区）= 已释放纹理 VA 被复用为权限不同的 BO（可见窗 swapchain dmabuf 被 KWin import
后参与复用 → 权限冲突；llm 隐藏窗 dmabuf 不被 import → 复用模式不同 → 读旧页 = 全黑无 fault）。
§十一"状态漂移/分钟级漂移/per-process"= **UAF 的非确定性**（每次运行的 VA 复用模式不同，
VMA free list 顺序 / KWin dmabuf 池 / 分配总量决定），非独立的驱动状态问题。

**为什么这个"把一行移出块"的 bug 查了数周（复盘）**：
1. UAF 伪装成驱动状态——VA 未复用时渲染**完全正常**（好状态 16/18 通过就是它在读旧数据），
   所有"时好时坏"证据都指向驱动/内核/合成器，无人怀疑自身对象生命周期；
2. §12.2 的 Mesa 源码分析（RADV 写时即解析 pView）**正确但带偏方向**——它排除了"描述符
   结构体指针"的生命周期嫌疑，但真正的受害者是**纹理 BO 本身**：image 析构后，heap slot 里
   自包含描述符编码的 GPU VA 照样悬空（写时解析只固化了"写那一刻的 VA"，固化不了"VA 的存活"）；
3. A–G 全在**已修复**的 master 上做单变量回退——根因在 main.cpp 作用域（"纯管道"，无任何
   A–G 变量覆盖它），回退任何被测变量都复现不出；
4. 2de6731 commit message 把修复标成"slot 顺序"（getWhite 动机），生命周期效果未记录——
   读 commit 的人全部误判为诊断设施；
5. CPU 侧工具全瞎：ASan/Valgrind 看不到 GPU 的访问，VVL 无对应 VUID（spec 不要求描述符
   指向的 image 存活），唯一证据是驱动层 GPUVM fault 日志——离真正的 bug（一个 C++ 析构）
   隔了 4 层抽象（C++ scope → VMA → BO/VA → 内核 PTE）。

**遗留验证（2026-08-28 傍晚已完成 → PASS）**：比合成 `TEXTURE_FORCE_SIZE=4096` 更硬的复验——
master 上换真实 4K 材质模型（`realistic_wooden_cottage_garden_house_3d_model.glb`，
257MB / 392 primitive / **11 张 4096×4096 64MB 内嵌 sRGB 纹理**，全部真实上传），
§十 4K 黑屏的精确配置（llm 隐藏窗 1280×720 = 目标 <2048）+ 可见窗交互两路复跑：
**截图木纹/屋顶/窗棂细节完整，零 fault**（`screenshots/cottage_4k_uaf_check.png`）。
→ **4K 黑屏 = 同根 UAF**（64MB 纹理 BO 第 0 帧前释放，其 VA 被复用到 high VA 段 → TCP 权限
fault/全黑；§十 lldb 观测的 "high VA BO" = 复用后的分配，非原始纹理）；§十"内核 gpuva
high VA + 48 位采样描述符编码"定性整体改写为 UAF 复用，**§10.7 的上游 bug report 不发**。

### 12.5 未解项（12.6 后更新）
- ~~**交互坏状态 frame 2 fault**（状态分钟级漂移 + per-process）~~ → **已解（12.6）**：根因 =
  TexturePool UAF（2de6731 修复）；"状态漂移" = UAF 非确定性；08-28 观测到 fault 的均为
  修复前构建（2de6731 开发过程中），修复后本日 10+ 轮运行（A–G + I/J 对照 + 现场复跑）无一次 fault；
- ~~**4K 黑屏**（64MB + 目标 <2048，§十）~~ → **已解（12.6 遗留验证完成）**：真实 4K 材质
  模型（cottage，11 张 4096² 64MB 内嵌纹理）在 §十 精确配置（目标 1280×720 <2048）+ 可见窗
  两路复跑均正常零 fault → 4K 黑屏 = 同根 UAF 复用；§十 定性整体改写，上游 bug report 不发；
  §11.5 group 数实验（10→128）随 frame 2 fault 定性为 UAF 而失去意义（group 数不是触发变量）。

### 12.7 教训：这类 GPU 侧 UAF 如何提前掐灭（2026-08-28 收尾总结）

**先回答"先上 ASan？"：这个 bug ASan 是瞎的**——ASan/Valgrind 拦 CPU 内存访问、redzone 在
CPU 地址空间；本 UAF 里 CPU 侧析构后再不碰悬空指针（RADV 写时即解析，§12.2），真正访问
已释放 BO 的是 **GPU 走自己的页表**（VA→PTE→物理页），完全绕开 CPU 分配器。ASan 对 GPU 侧
UAF 结构性失明——先上会得到"全干净"的假安慰（复盘第 5 条）。

**VVL 才是 Vulkan 生命周期 bug 的天然第一道防线，但它也漏了** → 真实的体系性缺口：普通
descriptor set 有"引用的 image 必须活过使用"的 VUID + VVL 跟踪；descriptor heap（新扩展）
的 **"heap slot 描述符是持久引用，image 必须活过引用"无对应跟踪**——本 bug 的 image 在
"帧间空隙"（无 in-flight command）析构，VVL 视角 = 合法；但 heap 描述符永远指着它。
**值得给 Khronos 报 VVL/spec gap（系统性的"提前掐灭"答案）**。

**可落地手段（按成本排）**：
1. **所有权规则（零成本，review 时）**："凡被描述符引用的资源，owner 必须是初始化层对象
   （Resources/AssetTable），禁止 main() 局部。"项目已有同款规则（raii 对象必须成员初始化）——
   TexturePool 以"普通类"身份溜过去。架构正解 = `TextureAsset` 进 AssetTable（Phase 4 原计划）：
   **模型 BDA 没死就是因为它在 AssetTable 里**，纹理没进去——这个不对称就是 bug 本身；
2. **texel 回读金丝雀（~10 行）**：项目已有 upload 期 texel 回读 DIAG；加一条"第 N 帧重读
   同一 texel 比对"——纹理 BO 死了，回读立即偏/fault，把"时好时坏"变成"帧 3 必炸"；
   对本类 bug 最便宜的决定性检测；
3. **upload 时记录每个 GPU 资源的 VMA 分配身份（地址+大小），fault 时反查**：08-27 lldb
   手里就有 BO 地址，缺的只是问一句"这个 BO 是我传的那块，还是复用来的？"；
4. **签名识别**："同二进制时好时坏 + 驱动层页表 fault" = UAF/竞态经典签名。
   **驱动层 fault ≠ 驱动 bug**——fault 只是非法访问的症状，非法性可能在用户侧（freed
   mapping）。看到 TCP permission fault，第一问应是"这个 VA 的 owner 是谁、还活着吗"，
   而不是"内核坏了吗"。

**ASan 的正确位置**：普通 C++ 内存错误（CPU 侧 crash/腐坏）仍是第一道防线（debug 构建
`-fsanitize=address` 很便宜）；但"时好时坏 + GPU fault"签名出现时必须与上述 1–4 并用——
GPU 侧 UAF 恰是它全盲的那一类。
