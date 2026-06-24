# VK_EXT_descriptor_heap 使用规范

## 一、快速检查清单

| 步骤 | 合规要求 | 文件位置 |
|---|---|---|
| 查询属性 | `VkPhysicalDeviceDescriptorHeapPropertiesEXT` | 初始化时 |
| 分配 buffer | `eDescriptorHeapEXT` + `eShaderDeviceAddress` | VMA |
| Reserved range | `reservedRangeSize >= minResourceHeapReservedRange` | `createDescriptorHeap` |
| 写入描述符 | `vkWriteResourceDescriptorsEXT`，`dst.size >= descriptorSize` | `writeDescriptors` |
| Shader 创建 | `VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT` + `layout = nullptr` | `shader.cppm` |
| Binding mapping | `VkDescriptorSetAndBindingMappingEXT` 覆盖所有 `[[vk::binding]]` | `shader.cppm` |
| Heap offset | `heapOffset` / `heapArrayStride` **以字节为单位** | `shader.cppm` |
| 同步 | `eHostWrite` → `eResourceHeapReadEXT` barrier | `vkrenderer.cppm` |
| 绑定 heap | `vkCmdBindResourceHeapEXT` + 正确 reserved range | `vkrenderer.cppm` |
| 绑定 heap 时机 | **必须在 `beginRendering` 之前** | `vkrenderer.cppm` |

---

## 二、Heap 创建（含 reserved range）

```cpp
VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProps{};
heapProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;

VkPhysicalDeviceProperties2 props2{};
props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
props2.pNext = &heapProps;
vkGetPhysicalDeviceProperties2(physicalDevice, &props2);

// ⚠️ 必须查询 reservedRange
uint32_t descriptorSize = (uint32_t)heapProps.bufferDescriptorSize;
uint32_t reservedSlots = (uint32_t)(
    (heapProps.minResourceHeapReservedRange + descriptorSize - 1) / descriptorSize
);
uint32_t capacity = userSlots + reservedSlots;
```

### 坑 1：reservedRange 不能为 0

规范 3.3 节：
> `reservedRangeSize` must be greater than or equal to the `*HeapReservedRange` limit.
> 字节 `[reservedRangeOffset, reservedRangeOffset + reservedRangeSize)` 必须由实现管理。

传入 `(0, 0)` 在某些驱动上静默失败。

### 坑 2：heap buffer 必须同时设置两个 usage flag

```cpp
// ✅ 正确
bufferInfo.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress
                 | vk::BufferUsageFlagBits::eDescriptorHeapEXT;

// ❌ 错误：缺少任何一个都会导致 undefined behavior
```

---

## 三、写入描述符

```cpp
vk::DeviceAddressRangeEXT range{
    bufferAddress,
    bufferSize
};

vk::ResourceDescriptorInfoEXT descInfo{};
descInfo.type = vk::DescriptorType::eStorageBuffer;  // 或 eUniformBuffer
descInfo.data.pAddressRange = &range;

vk::HostAddressRangeEXT dst{
    heap->getSlotCpuAddress<void>(slot),
    heap->getDescriptorSize()  // ⚠️ 必须 >= descriptorSize
};

device.writeResourceDescriptorsEXT(
    std::vector<vk::ResourceDescriptorInfoEXT>{descInfo},
    std::vector<vk::HostAddressRangeEXT>{dst}
);
```

### 坑 3：vk-hpp 必须传 `std::vector`

vk-hpp 的 `writeResourceDescriptorsEXT` 参数类型是 `ArrayProxy<const T>`，**不能直接传裸指针或 initializer list**。必须用 `std::vector` 构造临时对象。

### 坑 4：`pDescriptors[i].size` 必须 >= descriptor size

```cpp
// ✅ 正确
dst.size = heap->getDescriptorSize();  // 128 (bufferDescriptorSize)

// ❌ 错误：写不完整的描述符 → 几何体错乱
dst.size = 64;
```

---

## 四、Shader Binding Mapping

```cpp
VkDescriptorSetAndBindingMappingEXT mappings[2]{};

// Binding 0 → heap slot 8
mappings[0].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
mappings[0].descriptorSet = 0;
mappings[0].firstBinding = 0;
mappings[0].bindingCount = 1;
mappings[0].resourceMask = VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT;
mappings[0].source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
mappings[0].sourceData.constantOffset.heapOffset = 8 * descriptorSize;   // ⚠️ 字节
mappings[0].sourceData.constantOffset.heapArrayStride = descriptorSize;  // ⚠️ 字节
```

### 坑 5：heapOffset / heapArrayStride 必须是字节单位

`VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT` 的 `heapOffset` 和 `heapArrayStride` 是 **字节偏移**，不是 slot 索引。

```cpp
// ❌ 错误：传的是 slot 索引
mappings[0].sourceData.constantOffset.heapOffset = 8;
mappings[0].sourceData.constantOffset.heapArrayStride = 4;

// ✅ 正确：乘以 descriptorSize 得到字节偏移
mappings[0].sourceData.constantOffset.heapOffset = 8 * descriptorSize;
mappings[0].sourceData.constantOffset.heapArrayStride = descriptorSize;
```

### 坑 6：resourceMask 必须覆盖 shader 中实际使用的资源类型

验证层严格检查：每个 `[[vk::binding(N, M)]]` 的 SPIR-V 资源类型必须在 `resourceMask` 中有对应位。

- `StructuredBuffer<T>` → `VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT`
- `ByteAddressBuffer` → 类型不明确，用 `VK_SPIRV_RESOURCE_TYPE_ALL_EXT`
- `RWStructuredBuffer<T>` → `VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT`

### 坑 7：每个 shader stage 独立设置 mapping

`VkShaderDescriptorSetAndBindingMappingInfoEXT` 需要分别 chain 到每个 `VkShaderCreateInfoEXT.pNext`。mesh shader 的 mapping 不会自动传播到 fragment shader。

---

## 五、命令录制顺序

```cpp
// ① Host→GPU barrier（如果在 CPU 端修改了 heap 数据）
vk::MemoryBarrier2 barrier{};
barrier.srcAccessMask = vk::AccessFlagBits2::eHostWrite;
barrier.dstAccessMask = vk::AccessFlagBits2::eResourceHeapReadEXT;
barrier.srcStageMask  = vk::PipelineStageFlagBits2::eHost;
barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllGraphics;
cmd.pipelineBarrier2(vk::DependencyInfo{}.setMemoryBarriers(barrier));

// ② 绑定 heap
vk::DeviceAddressRangeEXT heapRange{ heapAddress, heapSize };
cmd.bindResourceHeapEXT(vk::BindHeapInfoEXT(
    heapRange,
    reservedRangeOffset,   // >= 0
    reservedRangeSize      // >= minResourceHeapReservedRange
));

// ③ Push data
cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(&data, sizeof(data))));

// ④ 开始渲染
cmd.beginRendering(renderingInfo);

// ⑤ 绑定 shader + 动态状态
cmd.bindShadersEXT(stages, shaders);
// ... viewport, scissor, depth, ...

// ⑥ 绘制
cmd.drawMeshTasksEXT(1, 1, 1);
cmd.endRendering();
```

### 坑 8：barrier 必须在 `beginRendering` 之前

`VK_PIPELINE_STAGE_2_HOST_BIT` 不能在 dynamic render pass 内部使用。barrier 和 `vkCmdBindResourceHeapEXT`、`vkCmdPushDataEXT` 全部放在 `beginRendering` 之前。

### 坑 9：heap 与 descriptor set 互斥

规范 3.3 节：
> Use of these commands is mutually exclusive with existing descriptor set or descriptor buffer state.

调用了 `vkCmdBindResourceHeapEXT` 后，所有 descriptor set / descriptor buffer 状态立即失效。反之亦然。

---

## 六、StructuredBuffer Stride 问题

### 坑 10：`StructuredBuffer<T>` 元素自动对齐到 16 字节

```hlsl
StructuredBuffer<Vertex> vertexBuffer;
```

HLSL/Slang 的 `StructuredBuffer` 要求元素 stride 是 16 字节的倍数（最小 16B）。

```cpp
// C++: sizeof(Vertex) = 24 bytes (float3×2)
struct Vertex { float position[3]; float color[3]; };
```

GPU 读取 `vertexBuffer[1]` 时从偏移 32 读取，但实际数据在偏移 24 → **数据完全错乱**。

**修复：C++ 和 shader 端同时补齐到 32 字节**

```cpp
// C++
struct Vertex {
    float position[3];
    float color[3];
    float _padding[2];  // 补齐到 32 字节
};
```

```hlsl
// HLSL/Slang — 自动对齐，无需额外操作
struct Vertex {
    float3 position;
    float3 color;
    float  _padding[2];  // 可选，但建议显式写出
};
```

### 坑 11：4 字节数据不要用 StructuredBuffer

```hlsl
// ❌ 错误：StructuredBuffer<uint> stride = 16B，实际数据 4B/元素
StructuredBuffer<uint> indexBuffer;

// ✅ 正确：ByteAddressBuffer 按字节偏移读取
ByteAddressBuffer indexBuffer;
// 使用：indexBuffer.Load(index * 4)
```

---

## 七、矩阵 layout 问题

### 坑 12：glm column-major vs Slang row-major

```cpp
// C++: glm::mat4 = column-major
drawArgs.mvp = proj * view * model;
```

```hlsl
// Slang: float4x4 = row-major
// mul(M, v) 实际计算 M^T * v → 透视失真
```

**修复方案：** 保持 C++ 端 `proj * view * model` 不变，shader 端用 `mul(matrix, vector)`。因为 `glm::mat4` 的 column-major 内存布局被 Slang 的 row-major 解释后自然互为转置，`mul(M, v)` 中的 M 实际上就是正确的矩阵。

如果不行，在 C++ 端 `glm::transpose()` 补偿。

---

## 八、Push Data + vk-hpp RAII

```cpp
cmd.pushDataEXT(vk::PushDataInfoEXT(
    0,  // offset in bytes
    vk::HostAddressRangeConstEXT(&drawArgs_, sizeof(drawArgs_))
));
```

### 坑 13：`vk::PushDataInfoEXT` 构造函数

vk-hpp 的 `vk::PushDataInfoEXT` 构造函数接受 `(uint32_t offset, vk::HostAddressRangeConstEXT data)`。注意 `HostAddressRangeConstEXT` 的 `address` 是 `const void*`，不能用 `vk::HostAddressRangeEXT` 替代。

### 坑 14：`writeResourceDescriptorsEXT` 的 `HostAddressRangeEXT`

对应地，写入描述符用 `vk::HostAddressRangeEXT`（非 const `void*`），和 push data 的 `HostAddressRangeConstEXT` 是不同的类型，**不能混用**。

---

## 九、最佳实践总结

### 1. 封装 DescriptorHeap RAII wrapper

```cpp
struct VmaDescriptorHeap {
    VmaBuffer bufferStorage;       // 管理 buffer 生命周期
    void* pMappedData;             // CPU 映射地址
    uint64_t gpuAddress;           // GPU 端地址
    uint32_t descriptorSize;       // 单个描述符字节大小
    uint32_t capacity;             // 总容量（含 reserved）
    uint32_t reservedSlots;        // reserved 占用的 slot 数

    template<typename T = void>
    T* getSlotCpuAddress(uint32_t slot) const {
        return reinterpret_cast<T*>(
            static_cast<char*>(pMappedData) + slot * descriptorSize
        );
    }
};
```

### 2. slot 编号规划

```
slots 0..reservedSlots-1 → 实现保留，不要使用
slot 0..N  → push data 映射区域（如果需要）
slot 8     → vertex buffer
slot 12    → index buffer
slot 16+   → 其他资源
```

### 3. 一次性分配，全程复用

规范建议：
> There may be a high synchronization cost for binding a new heap on some implementations.

**最佳实践：** 启动时分配一个足够大的 heap，应用生命周期内不切换。动态更新描述符通过 `vkWriteResourceDescriptorsEXT`（设备端）或 CPU mapped memory（主机端）完成。

### 4. descriptorSize 必须查询，不能硬编码

```cpp
// ❌ 错误：硬编码 128
constexpr uint32_t descriptorSize = 128;

// ✅ 正确：从物理设备属性查询
VkPhysicalDeviceDescriptorHeapPropertiesEXT props{};
// ... vkGetPhysicalDeviceProperties2 ...
uint32_t descriptorSize = (uint32_t)props.bufferDescriptorSize;
```

### 5. 始终使用 `vk::DependencyInfo` + `pipelineBarrier2`

弃用 legacy `vkCmdPipelineBarrier`。descriptor heap 专用访问标志：

```cpp
vk::AccessFlagBits2::eResourceHeapReadEXT   // shader 读取 resource heap
vk::AccessFlagBits2::eSamplerHeapReadEXT    // shader 读取 sampler heap
vk::AccessFlagBits2::eHostWrite             // 主机写入（写入描述符后同步用）
```

### 6. Binding mapping 与 reserved slot 不冲突

`VkDescriptorSetAndBindingMappingEXT` 中映射的 heap slot 编号是全局的，从 0 开始计数。reserved slots 占用了 0..reservedSlots-1，映射时跳过这些 slot 即可。

```cpp
// reservedSlots = 2，用户从 slot 8 开始使用
// heapOffset = 8 * descriptorSize  ✅ 合法
// heapOffset = 1 * descriptorSize  ⚠️ 在 reserved range 内，不要使用
```

### 7. vk-hpp 类型陷阱速查

| API | dst 类型 | data 类型 |
|---|---|---|
| `writeResourceDescriptorsEXT` | `std::vector<vk::HostAddressRangeEXT>` | `vk::DeviceAddressRangeEXT` |
| `writeSamplerDescriptorsEXT` | `std::vector<vk::HostAddressRangeEXT>` | `VkSamplerCreateInfo` |
| `pushDataEXT` | `vk::PushDataInfoEXT` | `vk::HostAddressRangeConstEXT` |

注意：
- `HostAddressRangeEXT`（可写）vs `HostAddressRangeConstEXT`（只读）
- vk-hpp 的 `ArrayProxy` 参数必须传 `std::vector`，不能传裸指针
- `writeResourceDescriptorsEXT` 的 `ResourceDescriptorInfoEXT` 中 `sType` 字段不需要手动设置（vk-hpp 自动填充）
