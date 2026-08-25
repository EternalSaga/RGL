# heap_texture_spike — descriptor heap 纹理采样最小验证（Phase 3 预研）

## 回答的问题

Phase 3（TexturePool + 模型纹理采样）前，验证 descriptor heap 管线能否承载 **image 描述符 + 内嵌 sampler + shader object 采样**：

- **Q1**：`imageDescriptorSize` vs `bufferDescriptorSize`——现有 slot 寻址（`slot * bufferDescriptorSize`）能否容纳 image 描述符？
- **Q2**：统一 slot 步长是否为 image binding 的合法 offset 倍数（`imageDescriptorAlignment`）？
- **Q3**：同一个 resource heap 混存 buffer（`pAddressRange`）+ image（`pImage`）描述符？
- **Q4**：`pEmbeddedSampler` 在 mesh+frag shader object 链上采样？

## 结果（RADV，2026-08-25）

**Q1 FAIL（32 > 16，设计约束）/ Q2 PASS / Q3 PASS / Q4 PASS**——像素级验证（4 校验点精确匹配）。
完整记录：[docs/heap_texture_research.md](../docs/heap_texture_research.md)。NVIDIA 侧待 dGPU 机器补测（命令见下）。

## 构建 / 运行

```bash
cmake -B build -S . -G Ninja && cmake --build build
cd build
# RADV（本机核显）
RADV_EXPERIMENTAL=heap ./heap_texture_spike
# NVIDIA（dGPU 机器，环境变量同 vulkan_practice 的 Justfile run-dgpu）
VK_DRIVER_FILES=$NV_ICD __NV_PRIME_RENDER_OFFLOAD=1 __NV_PRIME_RENDER_OFFLOAD_PROVIDER=3 \
  __GLX_VENDOR_LIBRARY_NAME=zink __VK_LAYER_NV_optimus=NVIDIA_only ./heap_texture_spike
```

退出码 0 = PASS（Q1 是设计约束，不影响退出码判定；Q3/Q4 决定）。

## 实现要点（v341 API，与主工程同一编译环境）

- **无窗口**：离屏 256×256（RGBA8 两色纹理 → mesh 全屏三角形采样 + SSBO 加 0.1 → `vkCmdCopyImageToBuffer` 回读 → CPU 比对）
- 系统 Vulkan 头/loader 为 **v341**（旧）：buffer 描述符走 `data.pAddressRange`（裸 BDA）、`vkWriteResourceDescriptorsEXT` 4 参（`VkHostAddressRangeEXT`）、`pEmbeddedSampler` 在 `sourceData.constantOffset` 内、`VkBindHeapInfoEXT` 单 `heapRange`
- 旧 loader 不导出扩展符号 → 全部经 `vkGetDeviceProcAddr` 运行时解析（主工程经 Vulkan-Hpp dispatcher 同理）
- 纹理描述符：`VkImageDescriptorInfoEXT{pView=<image view **create info**>, layout=SHADER_READ_ONLY_OPTIMAL}`（pView 要 create info 不是 handle）
- fragment 用 GLSL 组合 `sampler2D`（OpTypeSampledImage）——本机 glslang 11.2 拒绝 `sampler2D(texture2D, sampler2D)` 构造器；Slang 变体（`Texture2D<float>`+`samplerState`，OpTypeImage+OpTypeSampler 对）留主工程验证，内嵌 sampler 机制 spec 对两者同等适用
- 已知：VVL 09600 对 heap image 描述符的 layout 跟踪伪影（像素精确 + 关 VVL 干净运行，判定非真实违规），见调研文档 §七
