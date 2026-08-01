# Shader Reflection & Schema Validation — Design Plan

## 1. 背景与目标

### 当前架构
- **vulkan_practice**：ShaderObject + DescriptorHeap + DynamicState + PushDataEXT，scalarBlockLayout
- **编译**：`slangc` → SPIR-V，无 UBO/SSBO/DescriptorSet，所有 struct 数据推入 DescriptorHeap
- **C++20 模块**（`.cppm`），RAII 生命周期管理

### 目标
- 用 `slangc -reflection-json` 替代 spirv-cross 做 shader 反射
- Lua schema 在 build 时校验 shader 契约，不匹配则编译报错
- 面向未来：支持 material system、asset pipeline 的 shader 接口规范化
- **不复用 ShaderReflection 的任何现有代码**，仅吸收其设计思路

---

## 2. Slang Reflection 能力确认

`slangc -reflection-json` + `-fvk-use-scalar-layout` 已提供：
- 所有参数的 binding（space/index）、类型、access
- struct 字段级 offset、size、elementStride（scalar layout）
- entry point 的 stage、varying I/O semantic、location
- push constant buffer 布局

**实测结论**：scalar layout 下 Slang reflection JSON 的 offset 与 C++ `sizeof` 一致，无需额外 padding 计算。

### 编译命令
```bash
slangc input.slang \
  -target spirv \
  -capability spvDescriptorHeapEXT \
  -profile sm_6_6+spirv_1_6 \
  -fvk-use-scalar-layout \
  -o output.spv \
  -reflection-json output.reflection.json
```

---

## 3. 整体流程

```
.slang source
    │
    ▼
slangc → .spv + .reflection.json
    │
    ▼
Lua schema validator (Python, build step)
    ├── 读取 .reflection.json
    ├── 读取 schema.lua
    ├── 校验通过 → 继续 build
    └── 校验失败 → CMake error, build 中断
    │
    ▼
C++ layout struct generator (optional)
    ├── 读取 .reflection.json
    ├── 生成 .hpp 中的 struct + static_assert
    └── 生成到 build/generated_shaders/
```

---

## 4. Lua Schema 校验设计

### 4.1 校验什么

| 类别 | 校验内容 | 当前需求 | 未来扩展 |
|------|---------|---------|---------|
| PushConstants | 字段名、类型、offset、total size | 立即可用 | - |
| Struct Layout | 指定 struct 的字段白名单、类型 | 立即可用 | material data 校验 |
| Resource Binding | binding (space, index) 合法性 | 立即可用 | descriptor heap 分配规则 |
| EntryPoints | stage 类型、数量 | 立即可用 | - |
| Stage IO | mesh output ↔ fragment input 语义匹配 | 立即可用 | 多 pass pipeline |
| Parameter Blocks | 将来 UBO/SSBO 的 schema 定义 | - | 恢复 UBO 时启用 |

### 4.2 Schema 文件格式（Lua）

```lua
-- Schema example: cube_schema.lua

return {
    -- Shader 基本信息
    entry_points = {
        { name = "main", stage = "mesh", numthreads = {32, 1, 1} },
    },

    -- Push Constants 契约
    push_constants = {
        struct_name = "PushConstants",
        fields = {
            { name = "mvp", type = "float4x4" },
            { name = "vertexBufferAddress", type = "uint64" },
            { name = "indexBufferAddress", type = "uint64" },
        },
        max_size = 128,  -- 字节上限
    },

    -- Resource binding 规则
    bindings = {
        -- 当前 shader 可能没有 resource，留空或注释
        -- { kind = "structuredBuffer", space = 0, index = 0, optional = true },
    },

    -- Struct 字段白名单（用于校验 descriptor heap 推送数据）
    struct_whitelist = {
        {
            name = "Vertex",
            fields = {"position", "color"},
        },
        {
            name = "PushConstants",
            fields = {"mvp", "vertexBufferAddress", "indexBufferAddress"},
        },
    },
}
```

### 4.3 校验逻辑（Python 脚本）

```python
# schema_validator.py
import json
import sys

def validate(reflection_json_path, schema_lua_path):
    with open(reflection_json_path) as f:
        reflection = json.load(f)

    schema = load_lua_schema(schema_lua_path)  # Lua → dict

    errors = []

    # 1. 校验 entry point
    for ep_schema in schema.get("entry_points", []):
        found = find_entry_point(reflection, ep_schema["name"], ep_schema["stage"])
        if not found and not ep_schema.get("optional", False):
            errors.append(f"Missing entry point: {ep_schema['name']} (stage={ep_schema['stage']})")

    # 2. 校验 push constants
    push_schema = schema.get("push_constants")
    if push_schema:
        push_param = find_push_constant(reflection, push_schema["struct_name"])
        if push_param:
            actual_fields = push_param["type"]["elementType"]["fields"]
            schema_fields = push_schema["fields"]
            for sf in schema_fields:
                if not field_exists(actual_fields, sf["name"], sf["type"]):
                    errors.append(f"Push constant field mismatch: {sf['name']}")
            actual_size = push_param["type"]["elementVarLayout"]["binding"]["size"]
            if actual_size > push_schema["max_size"]:
                errors.append(f"Push constant size {actual_size} exceeds max {push_schema['max_size']}")
        else:
            errors.append(f"Push constant struct '{push_schema['struct_name']}' not found")

    # 3. 校验 struct 白名单
    for sw in schema.get("struct_whitelist", []):
        struct_def = find_struct_in_reflection(reflection, sw["name"])
        if struct_def:
            actual_names = [f["name"] for f in struct_def["fields"]]
            for extra in set(actual_names) - set(sw["fields"]):
                errors.append(f"Unexpected field '{extra}' in struct '{sw['name']}'")

    if errors:
        print("Schema validation failed:")
        for e in errors:
            print(f"  ✗ {e}")
        sys.exit(1)
    else:
        print("Schema validation passed ✓")
        sys.exit(0)
```

### 4.4 Lua 解析方案

**选项 A：Python `lupa`** — Python 中嵌入 Lua 解释器
- 优点：轻量、直接
- 缺点：需 pip install lupa

**选项 B：sol2 CLI 工具** — 编译一个最小 sol2 程序做 Lua → JSON 转换
- 优点：与现有 sol2 依赖一致，无需 pip
- 缺点：多一个构建目标

**选项 C：schema 改用 JSON** — 放弃 Lua，直接用 JSON schema
- 优点：零依赖
- 缺点：失去 Lua 的灵活表达能力

**推荐：选项 A（lupa）**。若 lupa 不便安装，备选方案 C（JSON schema）。

---

## 5. C++ Layout Struct 生成（可选）

### 5.1 为什么需要

Push constant 和 descriptor heap 推送数据的 struct 布局需要 C++ 端与 shader 端严格对齐。自动生成可避免手工维护。

### 5.2 类型映射

```
scalar float32 → float
scalar int32   → int32_t
scalar uint32  → uint32_t
scalar uint64  → uint64_t
vector 3 float32 → glm::vec3
vector 4 float32 → glm::vec4
matrix 4x4 float32 → glm::mat4
```

### 5.3 输出示例

```cpp
// Generated from cube.mesh.reflection.json
namespace RGL {
namespace CubeMesh {

struct PushConstants {
    glm::mat4 mvp;
    uint64_t vertexBufferAddress;
    uint64_t indexBufferAddress;
};

static_assert(sizeof(PushConstants) == 80,
    "PushConstants size mismatch with shader reflection");

}} // namespace RGL::CubeMesh
```

**注意**：生成的 `.hpp` 在 `.cppm` 中通过 `#include` 引入。

---

## 6. CMake 集成

### 6.1 修改 `compile_slang` 函数

```cmake
function(compile_slang SHADER_SOURCE OUT_SPV_PATH)
    set(REFLECTION_JSON "${OUTPUT_SPV}.reflection.json")
    
    add_custom_command(
        OUTPUT ${OUTPUT_SPV} ${REFLECTION_JSON}
        COMMAND ${SLANGC_EXECUTABLE} ${SHADER_ABS}
                -target spirv
                -capability spvDescriptorHeapEXT
                -profile sm_6_6+spirv_1_6
                -fvk-use-scalar-layout
                -I "${SHADER_DIR}"
                -o ${OUTPUT_SPV}
                -reflection-json ${REFLECTION_JSON}
        DEPENDS ${SHADER_ABS}
    )
    
    # Schema validation step
    add_custom_command(
        OUTPUT "${REFLECTION_JSON}.validated"
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/schema_validator.py
                --reflection ${REFLECTION_JSON}
                --schema ${SCHEMA_FILE}
        COMMAND ${CMAKE_COMMAND} -E touch "${REFLECTION_JSON}.validated"
        DEPENDS ${REFLECTION_JSON} ${SCHEMA_FILE}
    )
    
    # C++ struct generation step (optional)
    add_custom_command(
        OUTPUT "${OUTPUT_SPV}.hpp"
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/generate_layout.py
                --reflection ${REFLECTION_JSON}
                --output "${OUTPUT_SPV}.hpp"
        DEPENDS ${REFLECTION_JSON}
    )
endfunction()
```

### 6.2 目录结构

```
RGL/
├── tools/                          # 新增工具目录
│   ├── schema_validator.py         # Lua schema → reflection JSON 校验
│   ├── generate_layout.py          # reflection JSON → C++ struct
│   └── schemas/                    # Lua schema 文件
│       └── cube_schema.lua
│
├── vulkan_practice/
│   ├── shaders/
│   ├── CMakeLists.txt              # 修改 compile_slang
│   └── ...
│
└── ShaderReflection/               # Phase 3 后删除
```

---

## 7. 实施步骤

### Phase 1：基础设施
1. `tools/` 目录创建
2. `tools/schema_validator.py` — 解析 Lua schema + reflection JSON，输出校验结果
3. `tools/schemas/cube_schema.lua` — 当前 cube shader 的 schema
4. 修改 `vulkan_practice/CMakeLists.txt` 的 `compile_slang` — 增加 `-reflection-json` 和 `-fvk-use-scalar-layout`
5. CMake 添加 validation 自定义命令

### Phase 2：C++ struct 生成
6. `tools/generate_layout.py` — reflection JSON → C++ struct + static_assert
7. 集成到 CMake build step
8. 更新 `vkrenderer.cppm` 使用生成的 struct

### Phase 3：清理
9. Schema 支持 resource binding 校验规则
10. Schema 支持 stage IO 语义校验
11. 废弃并删除 `ShaderReflection/` 目录

### Phase 4：Material System（未来）
12. Schema 支持 material property → descriptor heap slot 映射
13. 运行时 schema 驱动的 descriptor 分配
14. Assimp → descriptor heap data 的转换管线

---

## 8. 待讨论问题

1. **Lua schema 放在哪里？** `tools/schemas/` 还是各项目的 `shaders/` 旁边？
2. **generate_layout.py 是否必要？** 当前 push constant struct 在 `types.slang` 和 `cube.cppm` 中手动维护。生成器价值在于未来 shader 增多时减少手工维护。
3. **lupa 依赖**：如果不想引入 lupa，可以用 sol2 写一个最小 CLI 工具做 Lua → JSON 转换，或者 schema 改用 JSON 格式。
4. **ShaderReflection 目录**：Phase 1 完成后即可标记 deprecated，Phase 3 彻底删除。
