# heap_texture_spike — descriptor heap 纹理采样最小验证（Phase 3 预研）

## 回答的问题

Phase 3（TexturePool + 模型纹理采样）前，验证 descriptor heap 管线能否承载 **image 描述符 + 内嵌 sampler + shader object 采样**：

- **Q1**：`imageDescriptorSize` vs `bufferDescriptorSize`——现有 slot 寻址（`slot * bufferDescriptorSize`）能否容纳 image 描述符？
- **Q2**：统一 slot 步长是否为 image binding 的合法 offset 倍数（`imageDescriptorAlignment`）？
- **Q3**：同一个 resource heap 混存 buffer（`pAddressRange`）+ image（`pImage`）描述符？
- **Q4**：`pEmbeddedSampler` 在 mesh+frag shader object 链上采样？

## 结果（2026-08-27 三驱动交叉测试：RADV 26.2.1 / Intel ANV 26.2.1 / NVIDIA 610.57.4）

**5 变体 × 3 驱动全部像素级 PASS**（4 校验点精确匹配；Q1 是设计约束不影响退出码）：

| 变体（`SPIKE_SHADER` / `SPIKE_SAMPLER`） | RADV | Intel | NVIDIA |
|---|---|---|---|
| `glsl`：组合 `sampler2D`（OpTypeSampledImage）+ 内嵌 sampler | PASS | PASS | PASS |
| `slang` + `heap`：分离 OpTypeImage+OpTypeSampler + 独立 sampler heap | PASS | PASS | PASS |
| `slang` + `heap` + `SPIKE_SAMPLER_FIELD=samplerHeapOffset`（旧字段对照） | PASS | PASS | PASS |
| `slang` + `embedded`：分离 + 内嵌 sampler | PASS | PASS | PASS |
| `slang_ctrl`：纯 `Load`/OpImageFetch（无 sampler 路径）+ buffer | PASS | PASS | PASS |

**结论：descriptor heap 的 texture/sampler 语义分离（image 走 SAMPLED_IMAGE 描述符 + sampler 走 sampler heap/内嵌）三驱动完整实现。**
Q1 跨驱动：`imageDescriptorSize`（RADV 64 / NVIDIA 32 / Intel 64）> `bufferDescriptorSize`（16/16/64）——统一步长须 `max(buffer, image)`（Intel 恰好 64=64）。
完整记录（含误诊复盘：标量 `Texture2D<float>` 的 R 通道 splat、`samplerHeapOffset` 字段误用、reserved range 越界 bug）：[docs/archive/heap_texture_research.md](../docs/archive/heap_texture_research.md) §九。

## 构建 / 运行

```bash
cmake -B build -S . -G Ninja && cmake --build build
# slangc：configure 时自动 FetchContent 拉 Slang 官方预构建 v2026.16（cmake/SlangFetch.cmake，
# 与主工程同源，sha256 pin，一次性下载到 build/_deps/）；$ENV{SLANGC_DIR} 可显式覆盖（离线用）
cd build
# 一键三驱动 × 五变体矩阵：
bash ../cross_test.sh
# 单驱动单变体：
RADV_EXPERIMENTAL=heap ./heap_texture_spike                                        # glsl 基线
VK_DRIVER_FILES=.../intel_icd.x86_64.json SPIKE_SHADER=slang SPIKE_SAMPLER=heap ./heap_texture_spike
VK_DRIVER_FILES=.../nvidia_icd.json __NV_PRIME_RENDER_OFFLOAD=1 \
  __NV_PRIME_RENDER_OFFLOAD_PROVIDER=3 __GLX_VENDOR_LIBRARY_NAME=zink \
  __VK_LAYER_NV_optimus=NVIDIA_only SPIKE_SHADER=slang ./heap_texture_spike
```

退出码 0 = PASS。变体环境变量：`SPIKE_SHADER=glsl|slang|slang_ctrl`、`SPIKE_SAMPLER=embedded|heap`、`SPIKE_SAMPLER_FIELD=heapOffset|samplerHeapOffset`（缺省 heapOffset = spec 正确用法；samplerHeapOffset 复现旧字段行为做对照）。

## 实现要点（v341 API，与主工程同一编译环境）

- **无窗口**：离屏 256×256（RGBA8 两色纹理 → mesh 全屏三角形采样 + SSBO 加 0.1 → `vkCmdCopyImageToBuffer` 回读 → CPU 比对）
- 系统 Vulkan 头/loader 为 **v341**（旧）：buffer 描述符走 `data.pAddressRange`（裸 BDA）、`vkWriteResourceDescriptorsEXT` 4 参（`VkHostAddressRangeEXT`）、`pEmbeddedSampler` 在 `sourceData.constantOffset` 内、`VkBindHeapInfoEXT` 单 `heapRange`
- 旧 loader 不导出扩展符号 → 全部经 `vkGetDeviceProcAddr` 运行时解析（主工程经 Vulkan-Hpp dispatcher 同理）
- 纹理描述符：`VkImageDescriptorInfoEXT{pView=<image view **create info**>, layout=SHADER_READ_ONLY_OPTIMAL}`（pView 要 create info 不是 handle）
- fragment 两种形态：GLSL 组合 `sampler2D`（OpTypeSampledImage）/ Slang 分离（`Texture2D<float4>`+`SamplerState`，OpTypeImage+OpTypeSampler 对，主工程同款）——**均三驱动验证 PASS**（2026-08-27）
- **Slang 纹理元素类型必须是 `float4`**（对应 RGBA8）：标量 `Texture2D<float>` 的 Sample 按语义返回标量，隐式转 float4 时 R 通道 splat——spike 首版因此误判"驱动只认组合绑定"（复盘见调研文档 §九.2）
- reserved range 在堆首 `[0, reservedSlots*slotSize)`（`reservedRangeOffset=0`，VUID 11232/11233；主工程 VmaDescriptorHeap 同布局）；用户槽 = `reservedSlots+i`——NVIDIA `minResourceHeapReservedRange` 96768B（3024 槽）使首版堆尾越界 bug 暴露，已修
- OpTypeSampler 映射的 sampler heap 偏移写 **`heapOffset`**（spec :1664-1665 + VUID 11253）；`samplerHeapOffset` 字段仅组合 image sampler 用（:1671-1674）——`SPIKE_SAMPLER_FIELD` 可复现旧用法对照
- 已知：VVL 09600 对 heap image 描述符的 layout 跟踪伪影（三驱动一致出现；像素精确 + 关 VVL 干净运行，判定非真实违规），见调研文档 §七
