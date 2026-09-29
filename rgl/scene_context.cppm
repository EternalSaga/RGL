module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>
export module scene_context;

import rlstd;

namespace RGL {

// 场景上下文契约（叶子模块）：ECS 组件 + registry context 值类型 + 视锥/包围球几何工具。
// **只有值类型**——system（animation/projection/bounds/renderList/帧循环骨架）不住这里
//（ecs.cppm / render_list.cppm），场景内容（cube 等）也不住这里。
// 独立成模块（2026-09-08 架构债务 2：原 `scene` 模块改名至此——它本就只含值类型）
// 目的 = 让机制层（render_list）与配置层（scene_config）依赖"数据契约"而非某个背着别的东西
// 的模块；组件单独成模块还避免了 ecs ↔ model_importer 的循环 import。
// 依赖面收敛：只 import rlstd（+ entt/glm 头），任何执行层模块都不得被本模块引用。

/**
 * 实体变换（local/world 矩阵，列主序）。
 * Phase 2 无层级嵌套：world = local（注册时写入）；父子关系见 Relationship。
 */
export struct Transform {
	glm::mat4 local{1.0f};
	glm::mat4 world{1.0f};  // Phase 2 无层级嵌套：world = local（注册时写入）
};

/**
 * 父子关系：与 legacy 同款手写组件（legacy/render_core/gl/EnTTRelationship.hpp，
 * entt 3.16 无内置 Relationship 头）。parent = entt::null 表示根。
 * 单向（E5.2）：children 不存储，由 rg.view<Relationship> 按 parent 过滤派生
 *（同一关系不再存两份，一致性不靠写入方自觉）。
 */
export struct Relationship {
	entt::entity parent = entt::null;
};

/**
 * 引用已上传 GPU 的资产（asset id = Resources 资产表的下标，见 vkresources.cppm 的 AssetTable）：
 * GPU 数据 / heap slot / 三角形数 / 管线条目引用全部收敛在资产表记录里，
 * entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args 的五方约定收敛为一处映射（E1.1/E5.1）
 * objectSlot（Phase 6 instancing）：**场景实例**级对象矩阵表下标（scene data @128，
 * shader readObjectMatrix 消费）。注册期从 NextObjectSlot 单调领取，同 asset 多实例
 *（同文件重复 entity = 共享上传的 instancing）各持独立槽——资产身份与实例变换分离
 *（ECS 规范 5）。
 */
export struct MeshRef {
	uint32_t asset = 0;
	uint32_t objectSlot = 0;
};

/**
 * 对象矩阵槽分配器（registry context，Phase 6 instancing；main 启动期 emplace）：
 * 容量契约 = kSceneMatrixCapacity（scene data @128 矩阵表，main 注册完成后检查）。
 */
export struct NextObjectSlot {
	uint32_t next = 0;
};

/**
 * 材质真引用（批次 2，E5.3 解决；roadmap Phase 2"暂时只存引用"兑现）：material =
 * Resources 资产表 materials 容器的下标（MaterialAsset，vkresources.cppm）——
 * baseColor / textureId / alphaMode 全部收敛进材质资产记录，组件只持纯数据引用
 *（项目"组件只存数据引用"约定）。每帧渲染经资产表解析（ecs.cppm renderListSystem）。
 * 无纹理/无 uv 的 primitive 有效纹理 = 白 1×1（有效选择不在组件，见 MaterialAsset 注释）。
 */
export struct MaterialRef {
	uint32_t material = 0;
};

/**
 * 点选选中的实体（Phase 10 批 10.5 picking / 10.7 高亮；registry context）。
 *
 * 住在 scene_context 而非 ecs.cppm：renderListSystem（机制层）每帧要读它填高亮通道
 * （FrameDrawList.hasSelection/selectedMaterial），若留在 ecs 会造成 ecs ↔ render_list
 * 循环 import（ecs_impl 已 import render_list）。本模块是叶子值类型层，正是它的位置。
 *
 * entity = entt::null 表示未命中/尚未点选/Esc 已取消——面板"选中"页与高亮 pass 同看
 * 这一个字段，不会出现「面板说有选中但画面不亮」的分裂态。
 * materialId = 该实体引用的材质（MaterialRef.material 同值）——高亮按**材质**过滤，
 * 故同材质全部实体一起亮（Phase 10 批 10.7 决策）。
 * 选中态不在 registry 外平行维护（ECS 规范 1）。
 */
export struct SelectedObject {
	entt::entity entity = static_cast<entt::entity>(entt::null);
	uint32_t objectSlot = 0;  // MeshRef.objectSlot 同值（GPU 回读 raw id − 1）
	uint32_t materialId = 0;  // MaterialRef.material 同值（高亮过滤键）
};

/**
 * 包围球（组件，挂在有包围球的 entity 上）：现状 = 模型整体球挂场景根（registerScene 写入）；
 * 聚合进 SceneBounds 由 boundsSystem 完成（ecs.cppm）。
 */
export struct Bounds {
	glm::vec3 center{0.0f};
	float radius{0.0f};
	/** 默认构造：空球（center 原点，radius 0）。 */
	Bounds() = default;
	/**
	 * 由球心与半径构造包围球。
	 * @param c 球心
	 * @param r 半径
	 */
	Bounds(glm::vec3 c, float r) : center(c), radius(r) {}
};

/**
 * 场景内容聚合包围球（registry context，boundsSystem 每帧由全部 Bounds 组件 union 推导；
 * radius=0 = 空场景）。projectionSystem 据此自适应推导投影 near/far（ecs.cppm）。
 */
export struct SceneBounds {
	glm::vec3 center{0.0f};
	float radius{0.0f};
};

/**
 * Phase 7 批次 4：逐图元节点局部包围球（component，registerScene 每 primitive entity 挂一个）。
 * 与 Bounds 分离：Bounds = 模型根世界聚合球（boundsSystem union 供 near/far），CullSphere = 逐 draw
 * 图元 node-local 球（render_list 每帧 modelMatrix 变换后测视锥），互不污染。数据源 = GltfPrimitive::bounds。
 */
export struct CullSphere {
	glm::vec3 center{0.0f};   // node-local（不含 nodeWorld，render_list modelMatrix 再乘）
	float radius = 0.0f;
};

/**
 * 视锥体（Gribb-Harman 6 平面提取，Vulkan clip z∈[0,1]——GLM_FORCE_DEPTH_ZERO_TO_ONE）。
 * 平面法线朝内，已按法线长度归一化，plane·(x,1) = 世界空间有符号距离。
 */
export struct Frustum {
	glm::vec4 planes[6];      // left/right/bottom/top/near/far
};

/**
 * 从 clip = proj·view（列主序）提取 6 面（Gribb-Harman）。第 r 行 = (m[0][r],m[1][r],m[2][r],m[3][r])。
 * near = 第 2 行（z≥0，[0,1] 约定，非 [-1,1] 的 row3+row2）。
 * @param m proj·view 矩阵（列主序，clip z∈[0,1]）
 * @return 6 平面视锥（法线朝内、按法线长度归一化；零法线平面原样保留）
 */
export inline Frustum extractFrustum(const glm::mat4& m) {
	const glm::vec4 row[4] = {
		glm::vec4(m[0][0], m[1][0], m[2][0], m[3][0]),
		glm::vec4(m[0][1], m[1][1], m[2][1], m[3][1]),
		glm::vec4(m[0][2], m[1][2], m[2][2], m[3][2]),
		glm::vec4(m[0][3], m[1][3], m[2][3], m[3][3]),
	};
	Frustum f;
	f.planes[0] = row[3] + row[0];  // left
	f.planes[1] = row[3] - row[0];  // right
	f.planes[2] = row[3] + row[1];  // bottom
	f.planes[3] = row[3] - row[1];  // top
	f.planes[4] = row[2];           // near（z≥0）
	f.planes[5] = row[3] - row[2];  // far
	for (auto& p : f.planes) {
		const float n = glm::length(glm::vec3(p));
		if (n > 0.0f)
			p /= n;
	}
	return f;
}

/**
 * 世界包围球 vs 视锥（保守：球心到任平面距离 < −r 才剔，跨界/含内含均判可见）。
 * @param f 视锥（extractFrustum 产物）
 * @param c 球心（世界空间）
 * @param r 半径
 * @return true = 可见（或跨界无法排除）
 */
export inline bool sphereInFrustum(const Frustum& f, const glm::vec3& c, float r) {
	for (const auto& p : f.planes)
		if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r)
			return false;
	return true;
}

/**
 * 光源（组件，Phase 7 灯 entity 化，ECS 规范 3：画不画 = 实体有无）。每灯一个 entity + 本组件，
 * main 启动期 C2b 段按 sceneConfig.lights 顺序创建（scene_config 保证非空：lights 段缺 → 回落
 * [light]）。index = scene.lua lights 列表位（正序契约，显式钉死不靠 view 迭代序）：
 * forward 只读 index 0 = 主光（写 scene data @0 头单灯，diff0）；延迟 light pass 按 index
 * 落槽遍历全灯（≤ kMaxLights）。type=0 定向（direction 有效，世界空间，消费时归一化）；
 * type=1 点光（position 有效，range 线性衰减）。color×intensity 预乘进 GpuLight.color。
 */
export struct Light {
	uint32_t index = 0;                         // scene.lua lights 列表位（正序契约：index 0 = 主光）
	glm::vec3 direction{0.35f, -1.0f, 0.25f};  // 定向光照射方向（世界空间，消费时归一化）
	glm::vec3 color{1.0f, 0.98f, 0.95f};
	float intensity = 3.0f;
	glm::vec3 ambient{0.03f, 0.03f, 0.035f};   // 场景共享 ambient（仅 index 0 生效，进 scene data @36）
	// Phase 7 D3 点光扩展（定向光默认 type=0 时 position/range 不使用）
	glm::vec3 position{0.0f};
	float range = 0.0f;
	uint32_t type = 0;                          // 0=定向，1=点光
};

/**
 * 环境贴图参数（registry context，Phase 5.5；main 启动期从 configs/scene.lua
 * environment 段 + env cubemap 上传结果 emplace，仿 Light）。renderListSystem 每帧
 * 写进 scene data 头 @48（SceneFrameData.envParams），env.slang readEnvParams 消费。
 * mipRange = cubemap mipLevels−1（上传期才知道，故 main 在 loadEnvCubeMapFiles 后填）；
 * 默认全 0 = 无环境（IBL 风格下环境项黑，direct light 不受影响）。
 */
export struct SceneEnv {
	float intensity = 0.0f;
	float mipRange = 0.0f;
	// Phase 8 收尾：tonemap 曝光（scene.lua 顶层 exposure；搭本 ctx 同路上传
	// scene data @56，tonemap.frag 消费——与 env 开关无关，任何变体的图都有 tonemap pass）
	float exposure = 1.0f;
	// Phase 11 批 11.4：SSAO 施加强度（scene.lua ao.strength；搭本 ctx 同路上传
	// scene data @60，tonemap.frag 的 lerp(1, ao, strength) 消费；0 = 逐位旁路）
	float aoStrength = 0.0f;
	// Phase 5.5 批次 4 天空盒：enabled = env 已上传且 scene.lua environment.skybox=true；
	// pipeline = ShaderRegistry "skybox" 条目下标（main 启动期 addSkyEntry 后回填；
	// 0xFFFFFFFF = 未建条目——renderListSystem 见 enabled 但 pipeline 无效 = 契约漂移）
	bool skybox = false;
	uint32_t skyboxPipeline = 0xFFFFFFFF;
};

/**
 * 场景整体时间旋转（registry context，main 启动期 emplace；animationSystem 每帧更新
 * angle/matrix）：speed=0 = 静止（configs/scene.lua 的 spin 开关）。
 * 现状承载"全场景同一旋转"（MVP = proj·view·matrix·world）；per-entity 旋转需求出现时
 * 改为写 Transform 组件，结构同构无需二次重构（ecs_compliance_review E4.1）。
 */
export struct SceneSpin {
	glm::vec3 axis{0.5f, 1.0f, 0.0f};
	float speed = 1.0f;   // rad/s
	float angle = 0.0f;   // 累积角（animationSystem: angle += dt * speed）
	glm::mat4 matrix{1.0f};  // = rotate(axis, angle)，animationSystem 每帧更新
};

/**
 * 每帧 view/proj（registry context，projectionSystem 由 Camera + SceneBounds + WindowExtent 推导）
 */
export struct ViewProjection {
	glm::mat4 view{1.0f};
	glm::mat4 proj{1.0f};
};

}  // namespace RGL
