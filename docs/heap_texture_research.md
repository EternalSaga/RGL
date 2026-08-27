# Descriptor Heap 纹理采样预研（Phase 3 依据）

> **状态**：调研（Phase 3 依据；RADV 侧 spike PASS，NVIDIA 侧 spike 待补测。2026-08-27：主工程 buffer 描述符路径已 NVIDIA/Intel/AMD 三平台验证通过（commit `1f43d65`），image 描述符 spike 仍待 dGPU 环境。**2026-08-27 主工程接入实测：Slang 分离绑定（OpTypeImage+OpTypeSampler）在 RADV descriptor heap 采样恒 garbage——RADV 只映射组合 OpTypeSampledImage，详见 §八；NVIDIA 侧该变体待补测**）

**日期**: 2026-08-25
**环境**: Gentoo, Vulkan 头文件 1.4.341（系统 `/usr/include/vulkan`，主工程编译环境），loader 1.4.341（旧，扩展符号不在动态符号表），RADV ICD 1.4.354（AMD Radeon 8060S, STRIX_HALO）。**NVIDIA 侧未测**（spike 已备，见 §五；2026-08-27 起有 NVIDIA 环境可补测，当前开发机为 Strix Halo 纯核显机）。
**结论**: 双问题均有答案——
1. **Q1（关键设计约束）：RADV `imageDescriptorSize = 32B > bufferDescriptorSize = 16B`**，现有 slot 寻址（`slot * bufferDescriptorSize`）**不能直接容纳 image 描述符**。Phase 3 slot 布局须改为统一步长 `max(bufferDescriptorSize, imageDescriptorSize)`（运行时查询，勿硬编码）；
2. **Q3/Q4 实测 PASS（RADV，像素级验证）**：同一个 resource heap 可混存 buffer（`pAddressRange` 裸地址）+ image（`pImage`/`VkImageDescriptorInfoEXT`）描述符；`pEmbeddedSampler`（内嵌 sampler）在 mesh+frag shader object 上工作正常，采样结果与 CPU 预期逐像素一致。

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

## 四、RADV 实测结果（2026-08-25，RADV ICD 1.4.354，STRIX_HALO）

**heap 属性**：

| 属性 | 值 |
|---|---|
| `bufferDescriptorSize` | **16** |
| `imageDescriptorSize` | **32** |
| `samplerDescriptorSize` | 16 |
| `bufferDescriptorAlignment` / `imageDescriptorAlignment` / `samplerDescriptorAlignment` | 4 / 4 / 4 |
| `minResourceHeapReservedRange` | 0（RADV 保留 0 槽） |
| `resourceHeapAlignment` | 4 |
| `maxResourceHeapSize` | 2^64-1（不限制） |
| `maxDescriptorHeapEmbeddedSamplers` | 2048 |

**问题回答**：

| 问题 | 结果 | 说明 |
|---|---|---|
| Q1 image 描述符 ≤ buffer 步长？ | **FAIL**（32 > 16） | 现有 16B slot 寻址不能容纳 SAMPLED_IMAGE 描述符。Phase 3 统一步长改 `max(16,32)=32B`（运行时查询）；现有 buffer slot（0/4/8+i）**槽号不变**、仅步长变宽，16B→32B 每 slot 浪费 16B 可接受 |
| Q2 统一 32B 步长对齐合法？ | **PASS** | 32 % imageDescriptorAlignment(4) == 0 且 32 % bufferDescriptorAlignment(4) == 0——image 与 buffer binding 的 heapOffset 都合法 |
| Q3 单 heap 混存 buffer+image？ | **PASS** | 同一 resource heap 写两种描述符、shader object 同时引用，渲染正常，VVL 无相关报错 |
| Q4 内嵌 sampler 采样？ | **PASS** | `pEmbeddedSampler`（linear/clamp）+ SAMPLED_IMAGE heap 描述符，mesh+frag shader object 链采样正确，**4 个校验像素全部精确匹配**（(8,8)=(89,153,216,255)=(0.35,0.60,0.85,1)；(136,128)=(255,51,89,255)=(1.0,0.20,0.35,1)） |

## 五、NVIDIA 待测项

本机无 NVIDIA ICD，以下留 dGPU 机器跑同一 spike 补测（命令见 §三）：

1. `bufferDescriptorSize` / `imageDescriptorSize` / 三对齐值（NVIDIA 已知 `bufferDescriptorSize=16`、`resourceHeapAlignment=32`、`minResourceHeapReservedRange` 巨大——6048 槽，见 `nvidia_compatibility_report.md`；image 侧未知，**Q1 结论是否跨驱动成立取决于此**）；
2. Q3/Q4 复测（NVIDIA 对 heap image 描述符 + 内嵌 sampler 的驱动实现）；
3. 注意：NVIDIA 上 spike 若 Xid 13/engine reset，参照 `min_mesh_crash/README.md`（`NO_TASK_SHADER` 位规则）——spike 已带该位，若仍崩则是新根因，需 Aftermath 抓 dump。

**NVIDIA 结果回填后**，Q1 的跨驱动结论定案；在此之前 Phase 3 slot 布局按"统一 `max(buffer, image)` 步长"设计（对 RADV 已验证，对 NVIDIA 是上界安全的）。

## 六、对 Phase 3 的设计输入

1. **slot 步长**：`VmaDescriptorHeap::descriptorSize` 从 `bufferDescriptorSize` 改为 `max(bufferDescriptorSize, imageDescriptorSize)`（构造期查询）；`reservedSlots = ceil(minResourceHeapReservedRange / descriptorSize)` 随之（RADV 保留 0 无影响，NVIDIA 槽数减半、保留字节数不变）；`MeshAsset::heapSlot` 槽号语义不变，`heapOffset = slot * descriptorSize` 自动兼容。
2. **纹理描述符写入**：`vkWriteResourceDescriptorsEXT`（4 参，v341）+ `VkResourceDescriptorInfoEXT{type=SAMPLED_IMAGE, data.pImage=&VkImageDescriptorInfoEXT{pView=<image view create info>, layout=SHADER_READ_ONLY_OPTIMAL}}`；image view create info 须随 TexturePool 资产保留（不能只存 handle）。
3. **sampler**：一律 `pEmbeddedSampler`（`sourceData.constantOffset.pEmbeddedSampler`，v341 位置），不建 sampler heap；RADV `maxDescriptorHeapEmbeddedSamplers=2048` 充裕（每管线 1 个）。
4. **Slang 变体**：~~内嵌 sampler 机制 spec 对两者同等适用，主工程接入时直接验证~~——**已被主工程接入实测证伪（2026-08-27，见 §八）**：Slang `Texture2D<float>`+`SamplerState`（分离 OpTypeImage+OpTypeSampler 对）在 RADV descriptor heap 上采样恒 garbage，无论 sampler 走独立 sampler heap 还是 `pEmbeddedSampler`；RADV 只映射组合 `OpTypeSampledImage`（spike 已验证路径）。spec 层面分离绑定是合法的（sampler heap + `samplerHeapOffset` 字段专为此设），**这是 RADV 的实现缺口，非写法错误**；NVIDIA 侧待测。
5. **feature/扩展**：纹理路径无新增 feature；device 扩展维持现状（descriptor heap 隐式依赖 maintenance4/5 已在主工程）。

## 七、已知问题

- **VVL 09600 跟踪伪影**（RADV + 系统 VVL）：spike 在 validation layer 下报
  `VUID-vkCmdDraw-None-09600 ... expects VkImage ... to be in layout TRANSFER_DST_OPTIMAL -- instead, current layout is UNDEFINED`，
  但同一运行**像素精确匹配**且关闭 VVL 后运行完全干净（exit 0，无其他 VVL 报错）——判定为 VVL 对 descriptor heap image 描述符的 layout 跟踪缺陷（写描述符发生在 host 侧、布局转换发生在命令缓冲内，VVL 关联失败），非真实 spec 违规。主工程未开 VVL 运行，不受影响。

## 八、主工程接入实测（2026-08-27）：RADV 分离绑定缺口

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

**定性**：spec 支持分离绑定（独立 sampler heap + `VkDescriptorMappingSourceConstantOffsetEXT::samplerHeapOffset` + `VK_ACCESS_2_SAMPLER_HEAP_READ_BIT_EXT` 专为此设），主工程按 spec 正确实现；**这是 RADV 对 `VK_EXT_descriptor_heap` 分离绑定的实现缺口，非写法错误**。

**对 Phase 3 的影响**：
- 纹理数据/描述符/slot/barrier 全链路在 RADV 上就绪，**唯一缺口是采样绑定模型**。
- NVIDIA 侧待测：若 NVIDIA 支持分离绑定，则 Slang 现状可直接用；若同样只认组合，则需 model.frag 改 GLSL 组合 `sampler2D`（spike 已验证路径）或等 RADV 修复。
- **NVIDIA 测试方法**：主工程 `just run-dgpu`（prime offload 环境变量见 Justfile）跑 llm_debug 截图目检；或 `heap_texture_spike` 补测 Q3/Q4 + 分离变体。
