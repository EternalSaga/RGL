# Shader Reflection & Schema Validation — Design Plan

> **状态**：历史（Phase 0 设计规划，2026-08 已实现并演进——实际落地形态：`tools/` 三模块 + inja 模板 + 两条反射管线（types/model），见 AGENTS.md 模块结构表与 `next_roadmap.md` Phase 0 完成情况。本文仅作设计思路留档，正文与现状代码不一致处以后者为准。）

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

### 4.3 校验逻辑（C++ 工具）

纯 C++ 实现，不引入 Python。两个独立 CLI 工具，编译到 `build/tools/`，CMake build step 调用。

#### 4.3.1 `schema_validator` — Lua Schema 校验器

**依赖**：sol2（已有）、nlohmann_json（已有）、Lua（已有）

```
build/tools/schema_validator \
    --reflection <reflection.json> \
    --schema      <schema.lua>
```

**架构**：
1. sol2 执行 `schema.lua`，获取 Lua table
2. 从 Lua table 提取校验规则 → C++ struct
3. nlohmann_json 读取 `reflection.json`
4. 逐条校验，错误输出到 stderr，exit code 1 中断 CMake build

**核心代码思路**：
```cpp
sol::state lua;
lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
// 沙箱：禁用 dofile/loadfile/package/require
lua["dofile"] = sol::nil;
lua["loadfile"] = sol::nil;
lua["package"] = sol::nil;
lua["require"] = sol::nil;

auto result = lua.script_file(schemaPath);
if (!result.valid()) { /* error */ }

sol::table schema = result;
auto reflection = json::parse(std::ifstream(reflectionPath));

// 校验 entry_points
for (auto& ep : schema["entry_points"].as<std::vector<sol::table>>()) {
    std::string name = ep["name"];
    std::string stage = ep["stage"];
    // 在 reflection["entryPoints"] 中查找匹配
}

// 校验 push_constants
if (schema.valid_key("push_constants")) {
    auto pc = schema["push_constants"].as<sol::table>();
    std::string structName = pc["struct_name"];
    // 在 reflection["parameters"] 中查找 pushConstantBuffer binding
}

// 校验 struct_whitelist
if (schema.valid_key("struct_whitelist")) {
    // 遍历白名单，对比 reflection 中的 struct 定义
}
```

#### 4.3.2 `generate_layout` — C++ Struct 生成器

**依赖**：nlohmann_json（已有）

```
build/tools/generate_layout \
    --reflection <reflection.json> \
    --output     <output.hpp>
```

读取 reflection JSON，类型映射，输出 `.hpp`。纯 JSON → string 处理，无 Lua 依赖。

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

### 6.1 工具构建

在父级 `CMakeLists.txt` 或独立 `tools/CMakeLists.txt` 中构建两个工具：

```cmake
# tools/CMakeLists.txt
add_executable(schema_validator
    schema_validator.cpp
    lua_sandbox.cpp
    lua_sandbox.hpp
)
target_compile_features(schema_validator PRIVATE cxx_std_23)
target_link_libraries(schema_validator PRIVATE
    sol2::sol2
    nlohmann_json::nlohmann_json
    rllogger
    ${LUA_LIBRARIES}
)
target_include_directories(schema_validator PRIVATE ${LUA_INCLUDE_DIR})

add_executable(generate_layout
    generate_layout.cpp
)
target_compile_features(generate_layout PRIVATE cxx_std_23)
target_link_libraries(generate_layout PRIVATE
    nlohmann_json::nlohmann_json
)
```

### 6.2 修改 `compile_slang` 函数

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
        COMMENT "Slangc: ${SHADER_FULL_NAME} -> ${SPV_NAME}"
    )

    # Schema validation step (depends on schema_validator tool)
    add_custom_command(
        OUTPUT "${REFLECTION_JSON}.validated"
        COMMAND $<TARGET_FILE:schema_validator>
                --reflection ${REFLECTION_JSON}
                --schema ${SCHEMA_FILE}
        COMMAND ${CMAKE_COMMAND} -E touch "${REFLECTION_JSON}.validated"
        DEPENDS ${REFLECTION_JSON} ${SCHEMA_FILE} schema_validator
    )

    # C++ struct generation step (optional, depends on generate_layout tool)
    add_custom_command(
        OUTPUT "${OUTPUT_SPV}.hpp"
        COMMAND $<TARGET_FILE:generate_layout>
                --reflection ${REFLECTION_JSON}
                --output "${OUTPUT_SPV}.hpp"
        DEPENDS ${REFLECTION_JSON} generate_layout
    )
endfunction()
```

### 6.3 目录结构

```
RGL/
├── tools/                          # 新增工具目录
│   ├── CMakeLists.txt              # 构建 schema_validator 和 generate_layout
│   ├── schema_validator.cpp        # Lua schema → reflection JSON 校验
│   ├── lua_sandbox.cpp             # sol2 Lua 沙箱（可复用 LuaSandbox 思路）
│   ├── lua_sandbox.hpp
│   ├── generate_layout.cpp         # reflection JSON → C++ struct
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
1. `tools/` 目录创建，`tools/CMakeLists.txt`
2. `tools/schema_validator.cpp` — sol2 解析 Lua schema + nlohmann_json 读取 reflection JSON，输出校验结果
3. `tools/lua_sandbox.cpp` — sol2 Lua 沙箱（思路复用自 ShaderReflection/LuaSandbox，但重写代码）
4. `tools/schemas/cube_schema.lua` — 当前 cube shader 的 schema
5. 修改 `vulkan_practice/CMakeLists.txt` 的 `compile_slang` — 增加 `-reflection-json` 和 `-fvk-use-scalar-layout`
6. CMake 添加 validation 自定义命令

### Phase 2：C++ struct 生成
7. `tools/generate_layout.cpp` — reflection JSON → C++ struct + static_assert
8. 集成到 CMake build step
9. 更新 `vkrenderer.cppm` 使用生成的 struct

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
2. **generate_layout 是否必要？** 当前 push constant struct 在 `types.slang` 和 `cube.cppm` 中手动维护。生成器价值在于未来 shader 增多时减少手工维护。
3. **schema_validator 的 sol2 依赖**：tools 需要链接 sol2 + Lua。现有 CMake 已经 find_package 了 sol2 和 Lua，可以直接复用。
4. **lua_sandbox 可复用性**：`tools/lua_sandbox.cpp` 的思路来自 `ShaderReflection/luaSandbox.cpp`，但代码会重写，适配 C++23 模块风格。
5. **ShaderReflection 目录**：Phase 1 完成后即可标记 deprecated，Phase 3 彻底删除。