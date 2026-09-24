# 灯 entity 化实施计划（ECS 规范 3）

> **状态**：待评审（设计底稿，未动代码）。
> **对应 roadmap**：`next_roadmap.md` Phase 7 剩余计划"**灯 entity 化**（ECS 规范 3）+ forward
> `LightDataGpu@0` 头并入反射"。
> **前置**：批次 3 多灯（灯数组 @262272 已入反射管线 4）、批次 5 GPU culling 均已完成——灯 entity 化
> 不依赖 culling（几何侧 vs 光照侧正交），主队列已清，可独立开工。
> **硬约束**：forward diff0（截图 BIT-IDENTICAL 是项目金标准）；GPU 契约（`GpuLight` 布局 /
> @262272 / 反射管线 4）逐字节不动。

---

## 0. 目标

把光源从 **registry context**（`SceneLights` = `vector<Light>`，启动期从 config emplace）升格为
**entity**（每灯一个 entity + `Light` 组件），使"场景里有哪些灯" = "registry 里有无该灯 entity"
（ECS 规范 3，同 cube 先例：`showCube` → 是否注册 cube entity，画不画 = 实体有无）。

改动面 = **灯的数据源从 config context 换成 entity**；GPU 契约完全不动，回归风险集中在 CPU 侧
灯数组构建序。

---

## 1. 现状数据流（事实 + 位置）

### 1.1 配置解析（`scene_config`，不变）

- `configs/scene.lua` `light` 段（定向光，含 `ambient`）→ `SceneConfig.light`
  （`scene_config.cppm:67`）。
- `lights` 段（多灯列表，**per-light 不解析 ambient**）→ `SceneConfig.lights`
  （`scene_config.cppm:68`，解析 `scene_config_impl.cppm:179-199`）。
- **回落**：`lights` 段缺 → `cfg.lights = [cfg.light]`（合成单主光，`scene_config_impl.cppm:201-202`）。
- **当前场景实况**（`configs/scene.lua:116-121`）：只有 `light` 段（direction/color/intensity/
  ambient={0.25,0.25,0.28}），无 `lights` 段 → `cfg.lights` = 单灯（来自 light 段）。

> 结论：当前运行时恒有 **1 盏灯**（`lights[0]` = light 段）。entity 化后必须仍产出这 1 盏、同值、
> 同序 → 才能 BIT-IDENTICAL。

### 1.2 context 填充（`main.cpp:200`，将删）

```cpp
registry.ctx().emplace<SceneLights>(SceneLights{sceneConfig.lights});
```

`SceneLights`（`scene_context.cppm:178-180`）= `std::vector<Light> lights`。

### 1.3 消费（`render_list_impl.cppm:132-148`，将改）

```cpp
const auto& sceneLights = rg.ctx().get<SceneLights>();           // :32
const Light primary = sceneLights.lights.empty() ? Light{}
                       : sceneLights.lights[0];                   // :132 forward 主光
fresh.sceneData.light = LightDataGpu{                            // :133 forward @0 头
    glm::normalize(primary.direction), camera.position,
    primary.color * primary.intensity, primary.ambient};
const uint32_t nLights = std::min<uint32_t>(
    static_cast<uint32_t>(sceneLights.lights.size()), kMaxLights);  // :138
fresh.sceneData.lightCount = nLights;
for (uint32_t i = 0; i < nLights; ++i) {                          // :141 deferred 灯数组
    const Light& L = sceneLights.lights[i];
    GpuLight& G = fresh.sceneData.lightArray[i];
    G.direction = glm::vec4(glm::normalize(L.direction), 0.0f);
    G.position  = glm::vec4(L.position, 0.0f);
    G.color     = glm::vec4(L.color * L.intensity, 0.0f);
    G.params    = glm::vec4(L.range, static_cast<float>(L.type), 0.0f, 0.0f);
}
```

两条消费路径：
- **forward**：只读 `lights[0]` → `LightDataGpu`（scene data @0 头，48B：direction@0/eyePos@12/
  color@24/ambient@36，`vkresources.cppm:41-47`）。**diff0 关键** = 主光必须仍是"列表第 0 个"。
- **deferred**：遍历全数组 → `GpuLight` 灯数组（scene data 尾区 @262272，stride 64，`kMaxLights=8`，
  契约 `lights.slang`，C++ 侧 `generated/lights_types_generated.cppm`）。

### 1.4 entity 范式参照（灯 entity 照此办）

- `registerScene`（`model_importer_impl.cppm:230-317`）：`rg.create()` + `rg.emplace<Transform/
  MeshRef/MaterialRef/Relationship/CullSphere>(e)`；`MeshRef.objectSlot` 从 `NextObjectSlot` 单调领取。
- `registerBuiltinCube`（`builtin_geometry_impl.cppm:134`）：同构。
- 灯 entity **不涉及资产表 / 上传 / 矩阵槽**（灯无 GPU 资产，数据直接进组件）——比 model/cube 更简。

---

## 2. 设计

### 2.1 组件与 entity

- **`Light` 结构体加 `index` 字段**（`scene_context.cppm:162`），升格为组件。`Light` 是 CPU 侧
  结构（非 GPU 布局——GPU 侧是 `GpuLight`/`LightDataGpu`），加字段安全。
  ```cpp
  export struct Light {
      uint32_t index = 0;    // scene.lua lights 列表位（正序契约：index 0 = 主光）
      glm::vec3 direction{0.35f, -1.0f, 0.25f};
      glm::vec3 color{1.0f, 0.98f, 0.95f};
      float intensity = 3.0f;
      glm::vec3 ambient{0.03f, 0.03f, 0.035f};   // 场景共享 ambient（仅 index 0 生效，@36）
      glm::vec3 position{0.0f};
      float range = 0.0f;
      uint32_t type = 0;                          // 0=定向，1=点光
  };
  ```
- **删除 `SceneLights` 结构体**（`scene_context.cppm:178-180`）——其唯一职责（持灯数组）被
  "一组 Light entity"取代。
- **`index` 为何进组件而非依赖 view 迭代序**：entt view 迭代序**无保证**（不得依赖）。`index`
  显式钉死"第 i 个灯 → 灯数组槽 i / 主光 = index 0"，正序契约不靠迭代序（同 `MeshRef.objectSlot`
  显式持槽的既有风格）。

### 2.2 entity 创建（`main.cpp` C 段新增，`sceneConfig.lights` → entities）

位置：C2 cube 注册循环之后、C3 GPU culling 之前（灯与 culling 无关，仅须在 renderListSystem 首帧
前存在——全在启动期，满足）。**不新建模块**（灯无资产/上传，逻辑 = 循环 create+emplace，内联最简）：

```cpp
// C2b：灯 entity 注册（ECS 规范 3：画不画 = 实体有无；index = lights 列表位，正序契约）。
// 数据源 = sceneConfig.lights（scene_config 已保证非空：lights 段缺 → 回落 [light]）。
for (uint32_t i = 0; i < sceneConfig.lights.size(); ++i) {
    auto e = registry.create();
    auto& L = registry.emplace<Light>(e);
    L = sceneConfig.lights[i];   // 全量拷贝灯数据（含 ambient）
    L.index = i;                 // 正序契约：index 0 = 主光（forward @0 头）
}
```

同时**删除** `main.cpp:200` 的 `emplace<SceneLights>`。

### 2.3 消费改写（`render_list_impl.cppm`）

`view<Light>` 收集（**不依赖迭代序**，按 `index` 落槽）：

```cpp
// 灯 entity 化：primary = index 0（forward @0 头），灯数组按 index 落槽（deferred）
std::array<Light, kMaxLights> lightSlot{};
Light primary{};
uint32_t lightEntityCount = 0;
rg.view<Light>().each([&](const Light& L) {
    ++lightEntityCount;
    if (L.index == 0) primary = L;
    if (L.index < kMaxLights) lightSlot[L.index] = L;
});
const uint32_t nLights = std::min(lightEntityCount, kMaxLights);
fresh.sceneData.light = LightDataGpu{
    glm::normalize(primary.direction), camera.position,
    primary.color * primary.intensity, primary.ambient};   // primary 默认 Light{} = 无灯时同旧 empty() 分支
fresh.sceneData.lightCount = nLights;
for (uint32_t i = 0; i < nLights; ++i) {
    const Light& L = lightSlot[i];
    GpuLight& G = fresh.sceneData.lightArray[i];
    G.direction = glm::vec4(glm::normalize(L.direction), 0.0f);
    G.position  = glm::vec4(L.position, 0.0f);
    G.color     = glm::vec4(L.color * L.intensity, 0.0f);
    G.params    = glm::vec4(L.range, static_cast<float>(L.type), 0.0f, 0.0f);
}
```

**diff0 等价性论证**（当前 1 灯场景）：
- 旧：`primary = lights[0]`（light 段值），`nLights = min(1,8)=1`，`lightArray[0] = lights[0]`。
- 新：唯一 entity `index=0` → `primary = 该灯`（同值），`lightEntityCount=1` → `nLights=1`，
  `lightSlot[0]=该灯` → `lightArray[0]`（同值）。
- scene data 逐字节相同 → forward/deferred GPU 输出 **BIT-IDENTICAL**。

### 2.4 ambient 处置（保留在组件，暂不拆分）

`Light.ambient` 是"场景共享但 per-light 存储（仅 index 0 生效）"的设计异味。entity 化**不改语义**：
仍存组件、仍由 `primary`（index 0）读出进 @36。理由：
- 当前 config 仅 1 灯（light 段），ambient 恒来自它——语义不变。
- 拆成场景级 `Ambient` 组件是更大重构，超出"灯升格 entity"范围，留后续（见 §6 边界）。

---

## 3. 改动清单（逐文件）

| 文件 | 改动 |
|---|---|
| `rgl/scene_context.cppm` | `Light` 加 `uint32_t index`（:162）；**删** `SceneLights`（:178-180）；更新 :156-171 / :173-177 注释（"context 元素"→"组件"，规范 3 落地注记） |
| `rgl/main.cpp` | **删** `:200` `emplace<SceneLights>`；C 段新增 C2b 灯 entity 循环（cube 循环后、C3 前） |
| `rgl/render_list_impl.cppm` | `:32` 删 `get<SceneLights>`；`:132-148` 改 `view<Light>` 收集（§2.3）；`:130` 注释更新 |
| `rgl/render_list.cppm` | `:76` 注释 `SceneLights` → `Light 组件` |
| `rgl/scene_config.cppm` / `_impl.cppm` | **不变**（`cfg.light`/`cfg.lights` 仍是配置侧真相源，回落逻辑保留） |
| `rgl/shaders/*` | **不变**（GPU 契约 `GpuLight`/`LightDataGpu` 逐字节不动） |

> 反射管线 / schema / 模板 **零改动**（`Light` 是 CPU 结构，非反射对象）→ 无需 `just reflect`。

---

## 4. 迁移步骤

1. **改 `scene_context.cppm`**：`Light` 加 `index`；删 `SceneLights`；注释同步。
2. **改 `main.cpp`**：删 `:200` emplace；C 段加 C2b 灯 entity 循环。
3. **改 `render_list_impl.cppm`** + `render_list.cppm` 注释：`view<Light>` 收集。
4. **编译**：`just build`（`doc_check` 随构建跑，确认新 `index` 字段/删 `SceneLights` 后注释合规）。
5. **冒烟**：build 副本 `configs/runtime.lua` `headless=true` →
   `cd build/rgl && RADV_EXPERIMENTAL=heap timeout 8 ./rgl`，零 `[error]`；`just build` 复原配置。
6. **BIT-IDENTICAL 回归**（同会话双构建，`git stash` 基线 vs 改动）：
   - forward（默认）+ deferred（build 副本 `render_graph.lua` `variant="deferred"`）各截图比对。
   - 预期 **diff0**（§2.3 等价性）。
7. **VVL**：`no_vvl=false` 跑 forward + deferred，零 error。
8. **多灯边界验证**（临时，验证后复原 config）：build 副本 `scene.lua` 加 `lights` 段 2-3 灯
   （含 1 点光）→ 确认 `lightCount` = 灯数、deferred 逐灯点亮、`index` 落槽正确；复原。
9. **文档同步**：`AGENTS.md` 已知问题（删"灯未 entity 化"项）、`next_roadmap.md`（批次 3 剩余
   "灯 entity 化"划掉 + 剩余计划更新）、`docs/调试历史.md`（若回归有发现）、`scene_context.cppm`
   头注释。
10. **提交**（用户确认后）。

---

## 5. 回归验收

- **BIT-IDENTICAL**：forward + deferred 截图同会话双构建 diff0（当前 1 灯场景，§2.3 论证）。
- **VVL 零 error**：forward + deferred。
- **灯数/主光正确**：启动 log 或诊断确认 `lightCount` = 灯数、主光 = index 0（forward @0 头值 =
  light 段值）。
- **多灯边界**（§4.8）：`lights` 段 2-3 灯时 deferred 逐灯正确、`index` 落槽、`kMaxLights` 钳制。
- **配置复原**：build 副本临时改动（runtime/render_graph/scene）全部被下次 `just build` 复原。

---

## 6. 风险与边界

- **view 迭代序**：已用显式 `index` 规避（不依赖迭代序）——唯一真实风险点，已封死。
- **`kMaxLights` 钳制**：灯 entity 数 > 8 时只取 `index < 8`（同旧 `min(size, kMaxLights)`）；
  当前场景 1 灯，无压力。
- **ambient 异味**：保留在组件（§2.4），不扩大改动面；场景级 `Ambient` 组件拆分留后续。
- **动态场景**：现状灯静态（启动期创建、运行期不改）。未来"运行时增删灯"= 增删 entity，
  `view<Light>` 每帧重收集天然支持——本次设计已留该接缝（`lightEntityCount` 每帧算，非启动期常量）。

---

## 7. 后续（Phase 2，独立项）：forward `LightDataGpu@0` 头并入反射

roadmap 与本 item 绑定但**正交**：消除 `LightDataGpu`（`vkresources.cppm:41-47`）手工双源债
（C++ struct ↔ `scene.slang` 字面量偏移手工一致，无反射端，AGENTS 已知问题）。

- **范围**：把 forward 灯头（@0 direction/@12 eyePos/@24 color/@36 ambient，48B）纳入反射管线
  （新管线或扩管线 4），生成 C++ struct 替换手写 `LightDataGpu`，shader 端 `readSceneLight()`
  改读生成契约。
- **与 entity 化的关系**：无依赖（entity 化改"灯数据从哪来"，Phase 2 改"`LightDataGpu` 怎么定义"）。
  建议 entity 化（Phase 1）BIT-IDENTICAL 验证通过后单独开工。
- **注意**：`eyePos`（@12）= 相机世界位置（每帧变），非灯字段——并入反射时它是"头布局"一部分
  但数据源是 Camera，需明确契约归属（避免把相机位置误当灯数据）。
- **验收**：同样 BIT-IDENTICAL（forward diff0）+ `just reflect` 重生成 + static_assert 三方同步。

> Phase 2 细节（反射管线怎么扩、shader 端怎么改）另开小节或独立文档，待 Phase 1 落地后细化。
