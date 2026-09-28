# Slang C API 直接反射（替代 slangc CLI → JSON 中间层）

> **状态**：现行（2026-09-10 落地，分支 `research/slang-api-reflection`；spike = `slang_api_spike/`，
> 工具 = `tools/slang_reflect.cppm` + `reflection_tool slang` 子命令，5 条管线已切 C API 路径，
> forward BIT-IDENTICAL 维持）。
> 触发：Phase 7 前向头 `SceneLight` 反射时发现——slangc 反射 JSON **不捕获非资源 struct**（`GpuLight`/
> `SceneLight` 走 schema `vertexLayouts` 声明 + 生成，无反射端数据，C++↔shader 双源纯手工信任）。
> 调研目标：链接 `libslang.so` 经 C API 直接查询编译器**实际**布局，真消除双源。

## 结论

**可行，且是严格升级。** Slang C API 能直接查出非资源 struct 的编译器实际布局（size/align/字段偏移/
字段类型，含跨模块），外加全局资源 binding 与 struct 静态成员常量值。spike 在真实 `scene.slang`/
`lights.slang` 上实证（见下）。这意味着反射工具可**不消费 JSON、直接消费 slang**——C++ struct 由
编译器实际布局生成，`static_assert(sizeof/offsetof)` 在 C++ 编译期交叉校验，双源真消除。

## 问题：现 JSON 管线的盲区

现管线（`rgl/CMakeLists.txt` 5 条）：`slangc -no-codegen -reflection-json → reflection_tool
validate/generate（消费 JSON + schema）`。反射 JSON 只捕获**资源**（binding 段）；
`tools/validator.cppm` 明说两处**无反射端对应**、纯 schema 手工信任：

- `vertexLayouts`（L196）："slangc 不反射顶点 buffer 结构体"——`GpuLight`/`SceneLight` 等 struct
  的布局由 schema 声明，C++ 由 schema 生成，**shader 侧须手工与 schema 一致**（双源）。
- `constants`（L229）："无反射端对应，只校验内部一致性"——shader 字面量须手工与 schema 一致（双源）。

`descriptorSlots`（binding）是唯一真实校验的段。前向头 `LightDataGpu`（`vkresources.cppm` 手写）↔
`scene.slang SceneLight` 即此双源的具体实例。

## API 面（slang.h 2026.16，预构建包自带 `include/` + `lib/libslang.so`）

编译流（与 slangc CLI 等价，`ICompileRequest` 在 `slang-deprecated.h`，仍可用）：

```cpp
slang::IGlobalSession* gsession; slang_createGlobalSession(SLANG_API_VERSION, &gsession);
slang::ISession* session;        SessionDesc{}; gsession->createSession(desc, &session);
slang::ICompileRequest* req;     session->createCompileRequest((SlangCompileRequest**)&req);
req->processCommandLineArguments(cli, n);  // 喂 SLANG_FLAGS 同款：-fvk-use-scalar-layout 等
req->compile();
slang::ShaderReflection* refl = slang::ShaderReflection::get(req);  // C 式 opaque → C++ helper
```

> 2026-09-24：`IGlobalSession::createCompileRequest` 已标 `[[deprecated]]`（-Werror 下炸）；
> 改走 **`ISession::createCompileRequest`**（非 deprecated，2026.16）——先 `createSession`
> 再在 session 上建 request，`ICompileRequest` 查询面不变。

查询面（`slang::` namespace）：

| 需求 | API | 备注 |
|---|---|---|
| 按名找 struct | `refl->findTypeByName("SceneLight")` → `TypeReflection*` | **跨模块可用**（lights.slang import scene 的 SceneLight 也查到） |
| struct 布局 | `refl->getTypeLayout(type)` → `TypeLayoutReflection*` | `getSize()/getAlignment()/getFieldCount()/getFieldByIndex(i)` |
| 字段偏移 | `field->getOffset()` / `field->getTypeLayout()->getType()` | **编译器实际算的**（scalar layout 下 vec3=12B，偏移 0/12/24/36） |
| 字段类型 | `TypeReflection::getKind()/getScalarType()/getColumnCount()/getRowCount()` | Struct/Vector/Matrix/Scalar 等 |
| 资源 binding | `refl->getParameterByIndex(i)->getBindingIndex()/getBindingSpace()` | 对应现 JSON descriptorSlots 段 |
| 常量值 | `refl->findVarByNameInType(structType, name)->getDefaultValueInt/Float()` | **仅 struct 静态成员**；顶层 `static const` 全局变量无 API |

scalar layout 对齐：CLI `-fvk-use-scalar-layout` 经 `processCommandLineArguments` 原样传入即可
（C API 侧对应 `TargetDesc.forceGLSLScalarBufferLayout` / `CompilerOptionName::GLSLForceScalarLayout`，
spike 走 CLI 通道，与 `SLANG_FLAGS` 逐字一致，零分叉）。

## 证据（spike 实测，`slang_api_spike/`）

真实 `scene.slang`（`SceneLight`）与 `lights.slang`（`GpuLight` + 跨模块 `SceneLight`），
flags 与 `SLANG_FLAGS` 一致：

```
struct SceneLight: size=48 align=4 fields=4
  [0] direction  offset=0    size=12  vec3
  [1] eyePos     offset=12   size=12  vec3
  [2] color      offset=24   size=12  vec3
  [3] ambient    offset=36   size=12  vec3
struct GpuLight: size=64 align=4 fields=4
  [0] direction  offset=0    size=16  vec4
  [1] position   offset=16   size=16  vec4
  [2] color      offset=32   size=16  vec4
  [3] params     offset=48   size=16  vec4
global params: sceneData binding=3 space=0
```

`SceneLight` 偏移 0/12/24/36 = C++ `LightDataGpu`（4×glm::vec3）逐字段一致；`GpuLight` 0/16/32/48
= C++ `GpuLight`（4×glm::vec4）一致。常量：`LightConstants.kMaxLights`（struct 静态成员）
`getDefaultValueInt → 8`。

## 升级设计（若落地）

反射工具从"消费 JSON"改为"链接 libslang.so 直接查 API"：

1. **struct 布局**：`findTypeByName` + `getTypeLayout` → 生成 C++ struct + `static_assert(sizeof==N)`
   + 逐字段 `static_assert(offsetof(f)==M)`。偏移 = 编译器实际布局，**C++ 编译期即校验 shader↔C++
   一致**，漂移即编译失败（现 schema 信任 → 真反射）。
2. **binding**：全局资源 `getBindingIndex/Space` 替代 JSON descriptorSlots 段。
3. **常量**：shader 侧常量挪为 struct 静态成员（如 `struct LightConstants { static const int
   kMaxLights = 8; }`），`findVarByNameInType` + `getDefaultValueInt` 提取；或保留 schema 声明（不劣于现状）。
4. **schema 角色收窄**：从"布局真相源"降为"提取清单"（要哪些 type/常量 + 输出 module 名），布局不再
   由 schema 声明、改由编译器给出。
5. **构建接线**：`reflection_tool` 链接预构建包 `lib/libslang.so` + `include/`（`SlangFetch.cmake`
   已解压到 `build/_deps/slang-prebuilt-src/`，零新增下载）；5 条管线去 `-reflection-json` 步骤。

**收益**：非资源 struct 双源（`GpuLight`/`SceneLight`/前向头/顶点布局）真消除；JSON 中间层消失；
常量（struct 静态成员化后）亦可反射。**风险**：① 常量需 shader 侧重构为 struct 静态成员（改契约
shader，须 BIT-IDENTICAL 验证）；② `ICompileRequest` 标 `[[deprecated]]`（2026.16 主推
`ISession`/`IComponentType`，但反射查询面在 deprecated 侧，短期够用、需跟踪上游）；③ 链接 libslang.so
使工具编译依赖预构建包（已在 `build/_deps/`，无新增外部依赖）。

## 落地记录（2026-09-10）

- [x] `tools/slang_reflect.cppm`：封装 C API（`SlangContext` RAII + `queryStruct`/`queryPushConst`/
  `queryGlobalParams`），GMF `#include <slang.h>`，链接预构建包 `libslang.so`。
- [x] `tools/generator.cppm`：`generateCppmFromSlang`（API 数据 → inja，逐字段 offsetof）+
  `validateSlangVertexLayout`/`validateSlangPushConst`（API 实查 vs schema 交叉校验）。
- [x] `reflection_tool slang` 子命令（`--shader`/`--schema`/`--output`/`--module-name`/
  `--template`/`--flags` 单串/`--include`）；`--flags` 用单串 split 绕开 boost 不吞 dash 参数。
- [x] 5 条管线切 C API 路径（`rgl/CMakeLists.txt`）：原 `slangc -reflection-json + validate +
  generate` 三步合一为 `reflection_tool slang` 单命令；**删除 4 个 `*_reflection.json`**（不再生成）；
  `REFLECTION_PAIRS` 去 JSON 输入。
- [x] BIT-IDENTICAL：forward md5 `59fb4c0f...` 不变（改动纯构建期：工具 + 编译期 offsetof 断言）。
- **生成物变化**：`GpuLight` +3 条、`ModelVertex` +2 条 `offsetof` static_assert（旧 JSON 路径只
  有总尺寸断言）；`DrawArgs`/`types` 无变化。struct 字段/尺寸/常量全部一致 = 等价或更严。

## 剩余应用（本升级的能力）

- [x] **前向头 `SceneLight`/`LightDataGpu` 双源**（本升级的原始动机，2026-09-10 落地）：
  新建第 6 条管线 `scene_forward`（`scene.slang` 的 `SceneLight` → `scene_forward_types_generated.cppm`，
  逐字段 offsetof 断言）→ 替换 `vkresources.cppm` 手写 `LightDataGpu`（4 处：vkresources/
  frame_draw_list/render_list_impl/vkrenderer_impl）。原 `LightDataGpu` 默认成员初始化为死代码
  （renderListSystem 每帧显式聚合初始化 `.light`），替换为无默认的生成 `SceneLight` 行为不变。
  **forward BIT-IDENTICAL（md5 `59fb4c0f...` 不变）**——C API 能力真正兑现"消除双源"。
