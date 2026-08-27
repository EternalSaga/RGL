# Descriptor Heap 纹理采样预研（Phase 3 依据）

> **状态**：调研（Phase 3 依据；**2026-08-27 三驱动交叉测试完成**：RADV / Intel ANV / NVIDIA × 5 变体（组合 / 分离+sampler heap / 分离+内嵌 sampler / 无 sampler 对照）全部像素级 PASS——descriptor heap 纹理/sampler 语义分离**三驱动均完整实现**。原 §八"RADV 只映射组合 OpTypeSampledImage"结论**被推翻**（根因 = spike shader 把 RGBA8 纹理误声明为标量 `Texture2D<float>`，Slang 语义下 R 通道 splat 到 4 通道，详见 §九）；主工程 buffer 描述符路径已三平台验证（commit `1f43d65`）；主工程 §八 "NaN 全黑"观察为**待重测项**（需 model.slang 改 `Texture2D<float4>` + 映射字段修正后，桌面环境 llm_debug 截图复验）。主工程接入实测历史见 §八（部分定性已被 §九 修正）。

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
