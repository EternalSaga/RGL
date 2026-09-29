module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>
// lookAt / ortho 属 gtc/matrix_transform.hpp（Phase 11 批 11.2 buildShadowVolume 用；
// 与 camera.cppm 同一处 include——GMF 头可达但名字不可见，不加就是"no member named"）
#include <glm/gtc/matrix_transform.hpp>
// glm::inverse 属 gtc/matrix_inverse.hpp（frustumBoundingSphere 的视轴球心 → 世界变换用）
#include <glm/gtc/matrix_inverse.hpp>
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
 * Phase 11 批 11.2：由视锥求**精确外接球**（闭式解）。视锥 8 角共球、球心在视轴上——
 * 结果只依赖 near/far/fov/aspect，**与相机姿态（yaw/pitch/位置）完全无关**，这是阴影
 * 稳定性的关键：半径随相机连续变化 = 光空间投影逐帧缩放 = 影子在地面上"游动"
 * （2026-09-29 用户实测：机位平移 0.06，井盖阴影时有时无）。
 * 8 角都在球面上 → 这就是最小外接球，保证**整个视锥在球内**。
 * @param view 相机 view（球心视空间 → 世界用其逆）
 * @param proj 相机 proj（级联子视锥的深度重映射已烘在里面的形态；透视专用）
 * @return {center, radius}；退化 proj（正交/奇异）返回半径 0
 */
export inline Bounds frustumBoundingSphere(const glm::mat4& view, const glm::mat4& proj) {
	// 闭式解（GLM 探针验证 4 组参数 8 角距 ≡ r，含本项目的 RH_ZO 与重映射子 proj）：
	// 视空间角点 (±z·tanHalf·aspect, ±z·tanHalf, −z) 到视轴上球心 (0,0,−zc) 等距 →
	//   zc = (f² − n² + rf² − rn²) / (2(f − n))，r = √((f − zc)² + rf²)。
	// near/far 从 proj 深度行反推（RH_ZO：ndc.z = −A − B/z_eye，A=proj[2][2]、B=proj[3][2]
	// → n = B/A、f = B/(1+A)），fov/aspect = 1/proj[1][1] 与 proj[1][1]/proj[0][0]。
	//
	// **前两版的教训**（→ docs/调试历史.md 5a-5/5a-6）：Eberly 增量法与 extractFrustum
	// 平面符号约定混用 → 球装不下视锥；改 8 角世界 AABB 法修好正确性但引入姿态依赖
	// （AABB 随 yaw 变，实测还比精确球大 16%）。闭式解既不碰符号约定也不碰姿态。
	const float A = proj[2][2];
	const float B = proj[3][2];
	if (A >= 0.0f || B >= 0.0f || A < -1.0f)
		return Bounds{glm::vec3{0.0f}, 0.0f};  // 非透视/退化 proj（正交 A=0；奇异防御）
	const float nearPlane = B / A;
	const float farPlane = B / (1.0f + A);
	if (!(farPlane > nearPlane))
		return Bounds{glm::vec3{0.0f}, 0.0f};
	const float tanHalf = 1.0f / proj[1][1];
	const float aspect = proj[1][1] / proj[0][0];
	const float cornerScale = tanHalf * std::sqrt(aspect * aspect + 1.0f);  // 角点离轴距/深度
	const float rn = nearPlane * cornerScale;
	const float rf = farPlane * cornerScale;
	const float zc = (farPlane * farPlane - nearPlane * nearPlane + rf * rf - rn * rn)
		 / (2.0f * (farPlane - nearPlane));
	// ×1.001 余量：float32 下"8 角距 = r"只在舍入内竞争（探针大 near/far 比时相对误差
	// ~1e-5），阴影侧的保守方向是宁大勿小（大 = 多收投影体，小 = 漏画）。
	const float radius = std::sqrt((farPlane - zc) * (farPlane - zc) + rf * rf) * 1.001f;
	const glm::vec4 center = glm::inverse(view) * glm::vec4(0.0f, 0.0f, -zc, 1.0f);
	return Bounds{glm::vec3{center} / center.w, radius};
}

/**
 * 阴影级联切分用的距离量化（向上取整到 1/8 格）。自适应 near/far 随相机移动**连续**变化
 * （实测两机位 f=1.3813 vs 1.4345）→ 闭式球半径仍连续变 → 影子游动的另一半根源。
 * 量化后小幅移动落同一桶 → 半径/splits **完全恒定**；跨桶瞬间跳一次（≤12.5% 半径，
 * 离散且低频，肉眼是"分辨率变细/粗"而非"影子滑动"）。CPU 建切片与 shader 选级阈值
 * 必须同用量化值（render_list_impl 两处同源调用），否则选级与内容错位。
 */
export inline float quantizeShadowDistance(float d) {
	return std::ceil(d * 8.0f) * 0.125f;
}

/**
 * Phase 11 批 11.2：级联数量与当前生效级数。
 *
 * **为何区分两个常量**：`kSceneShadowCascadeCount`（定义在 scene_data.cppm，**不在此复制**）
 * 是 **scene data 区容量**（一次性预留 4 槽，避免后续加级联时再动尾部基址）；
 * `kActiveShadowCascades`（定义在 scene_data.cppm，**不在此复制**）是**本帧实际渲染与采样用到的级数**。
 * 两者分离后，“预留 4 槽但只画 1 级”才是一行配置（详见 phase11_plan.md §10）。
 */
// 当前生效级数 = scene_data::kActiveShadowCascades（定义在那里，不在此复制）

/** 阴影 pass 名（图内通用名，不带分块号）。 */
export inline std::string_view shadowPassName() { return "shadow"; }

/** 阴影 pass 的 drawSource（FrameDrawList 段名 = "shadow" + 分块号）。
 *  分块号进字符串而不是固定 4 个名字面量：执行器的拓扑校验与 segmentSpan 都走
 *  同一条“前缀匹配”路径，kActiveShadowCascades 从 1 改 4 时无需动任何词表。
 * @param cascade 分块号
 * @return 段名（小字符串，调用点在图编译/录制期即消耗）
 */
export inline std::string shadowDrawSource(uint32_t cascade) {
	return "shadow" + std::to_string(cascade);
}

/**
 * Phase 11 批 11.2：**实用切分**与阴影矩阵构造。
 *
 * ## 切分方案：**practical split**（对数分布与等伽分布的 50/50 混合）
 * 两种纯空间分布都不可用：等伽分布让近处切片极薄（texel 粗得看不出阴影），
 * 对数分布让远处切片过薄（远景几个像素里没有一级级联）。mix = 0.5 是常见甜点。
 *
 * ## 为何不从 proj 矩阵反推 near/far 而是直接拿 ViewProjection::nearPlane/farPlane
 * 二者等价（它们就是 projectionSystem 实际生效的值），但 proj 矩阵反推需要一组奇性算术，
 * 而它不带来任何额外精度（直接拿字段不会漂）。字段由 projectionSystem 写入 = 与 proj 同源，不会漂。
 *
 * @param near 当前近截面
 * @param far 当前远截面
 * @param cascade 级号（0..count-1）
 * @param count 总级数
 * @return 该级的远截止平方距离
 */
export inline float cascadeSplitDistance(float near, float far, uint32_t cascade, uint32_t count) {
	if (cascade + 1u >= count)
		return far;
	const float fN = cascade / static_cast<float>(count);
	const float fN1 = (cascade + 1u) / static_cast<float>(count);
	const float logSplit = near * std::pow(far / near, fN);
	const float uniSplit = near + (far - near) * fN1;
	return logSplit * (1.0f - 0.5f) + uniSplit * 0.5f;
}

/**
 * Phase 11 批 11.2：把 proj 的近远平面换成另一对（供级联子视锥用）。
 *
 * GLM 零到一深度的透视 proj 深度映射为 `ndc = -A + B/t`（t = 视空间深度），
 * 其中 `A = proj[2][2] = f/(n-f)`、`B = proj[3][2] = -f·n/(f-n) = A·n`。只要把这两个系数换成
 * 新的 (n', f') 就得到与原视雾 **完全一致**的子视锥（x/y 分布不变，只换深度映射）。
 * 直接从矩阵反推需要额外的奇性分布，而这里直接重建两个系数——**且不会因为原矩阵其他部分的
 * 数值差异而静默算错**。
 *
 * @param proj 当前投影矩阵（仅读索引 2/3 的 z 行）
 * @param newNear 新近截面
 * @param newFar 新远截面
 * @return 新投影矩阵
 */
export inline glm::mat4 projWithDepthRange(const glm::mat4& proj, float newNear, float newFar) {
	glm::mat4 out = proj;
	const float A = newFar / (newNear - newFar);   // = f'/(n'-f')，与 GLM 透视同形
	out[2][2] = A;
	out[3][2] = A * newNear;                        // = A·n' = B'
	return out;
}

/** 一级阴影的**光空间体**：正交盒。XY 由该级切片球定（紧），Z 覆盖**全部投影体**。
 *
 * 为何必须把两者拆开（2026-09-29 修 bug 的要点）：盒的 XY 决定影子落在哪、贴多大，
 * 所以要贴着受光面收紧；而 Z 必须够深，装下所有**能照到这片受光面**的投影体。
 * 建筑比受光面高得多，只按视锥切片拟合的球（近距离机位下 r 仅 1.25）压根装不下它们
 * —— 那些图元会在剔除阶段被丢掉，于是没人往阴影图里画，整个广场一个投影都没有
 * （实测：阴影开关对拍只差 149 像素 / 0.016%，等于阴影没开）。
 * 另：光空间正交投影下影子沿光方向平移，**XY 不变**——所以盒的 XY 只需覆盖受光面，
 * 盒外投影体照不进盒内，无需为它们扩 XY。这是能把 XY 收紧的前提。
 */
export struct ShadowVolume {
	glm::mat4 viewProj;  // 阴影 mesh 与采样端共用（同一矩阵，否则深度比较无意义）
	glm::mat4 view;      // 仅剔除用：世界球心 → 光空间（XY 相对切片中心为 0）
	float xyRadius;      // 光空间 XY 半宽 = 切片球半径
	float eyeOffset;     // 眼点 = 切片中心 − 光向·eyeOffset
	float tMin, tMax;    // 沿光轴深度范围（相对切片中心，t = dot(P − 切片中心, 光向)）
};

/**
 * 建一级阴影的光空间正交盒。
 * @param light 主光（取 direction 定光空间基；只取方向，位置由 eyeOffset 定）
 * @param slice 该级子视锥的包围球（定 XY 半宽与受光面深度范围 [−r, r]）
 * @param casterMin 全部投影体世界 AABB 的最小角（定 Z 下界）
 * @param casterMax 全部投影体世界 AABB 的最大角（定 Z 上界）
 * @param mapSize 阴影图整幅边长（texel 吸附用）
 * @return 光空间体（slice.radius ≤ 0 时退化为零半径体 = 谁都进不来 = 无阴影）
 */
export inline ShadowVolume buildShadowVolume(const Light& light, const Bounds& slice,
	const glm::vec3& casterMin, const glm::vec3& casterMax, uint32_t mapSize) {
	const float r = slice.radius;
	if (r <= 0.0f)
		return ShadowVolume{glm::mat4{1.0f}, glm::mat4{1.0f}, 0.0f, 0.0f, 0.0f, 0.0f};
	const glm::vec3 d = glm::normalize(light.direction);
	const glm::vec3 up = (d.y > 0.99f || d.y < -0.99f) ? glm::vec3{0.0f, 0.0f, 1.0f}
							   : glm::vec3{0.0f, 1.0f, 0.0f};
	// 沿光轴取并：受光面占 [−r, r]（切片球），投影体占其 AABB 在光轴上的投影。
	// pad 一点余量，避免投影体正好压在近/远平面上被裁掉。
	const float pad = r * 0.02f;
	const float tMin = std::min(-r, glm::dot(casterMin - slice.center, d)) - pad;
	const float tMax = std::max(r, glm::dot(casterMax - slice.center, d)) + pad;
	const float eyeOffset = -tMin;
	const glm::mat4 view = glm::lookAt(slice.center - d * eyeOffset, slice.center, up);
	const glm::mat4 proj = glm::ortho(-r, r, -r, r, 0.0f, tMax - tMin);
	// texel 吸附：平移必须加在投影**之前**（proj·T·view）——加在之后是裁剪空间单位，
	// 等于把位移放大了一倍半边长（5a-1 实测把视锥角点顶到 NDC 1.43 越界，教训全文 →
	// docs/调试历史.md 2026-09-29 ⑧）。
	const glm::vec4 clip = proj * view * glm::vec4(slice.center, 1.0f);
	const glm::vec2 ndc = glm::vec2(clip) / clip.w;
	const glm::vec2 snapped = (glm::round((ndc * 0.5f + 0.5f) * static_cast<float>(mapSize))
				 + glm::vec2(0.5f))
		/ static_cast<float>(mapSize) * 2.0f - glm::vec2(1.0f);
	return ShadowVolume{proj * glm::translate(glm::mat4{1.0f},
		glm::vec3((snapped - ndc) * r, 0.0f)) * view,
		view, r, eyeOffset, tMin, tMax};
}

/**
 * 投影体是否影响该级光空间体（逐级剔除）。判据 = 图元世界球投到光空间后与盒相交。
 * @param v 该级光空间体
 * @param worldCenter 图元世界球心
 * @param worldRadius 图元世界球半径
 * @return true = 该图元要画进这一级的阴影图
 */
export inline bool casterInShadowVolume(const ShadowVolume& v, const glm::vec3& worldCenter,
	float worldRadius) {
	const glm::vec3 ls = glm::vec3(v.view * glm::vec4(worldCenter, 1.0f));
	// XY：球在垂直于光向的平面上的投影仍是同半径圆盘，故直接用 ±worldRadius
	if (std::abs(ls.x) > v.xyRadius + worldRadius || std::abs(ls.y) > v.xyRadius + worldRadius)
		return false;
	// Z：view 把切片中心放在眼点前方 eyeOffset 处（view.z = −eyeOffset），故 t = −ls.z − eyeOffset
	const float t = -ls.z - v.eyeOffset;
	return t + worldRadius > v.tMin && t - worldRadius < v.tMax;
}

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
	// Phase 11 批 11.2：当前投影 near/far（**由 projectionSystem 写入**，不是配置原值）。
	// 为何需要：级联切分要按近远平分这个当前尺寸的远距区间，而 proj 矩阵里深度
	// 映射的系数反推出来需要知道它对应哪个 near/far（见 scene_context::projDepthRange）。
	// 自适应 near/far（按场景包围球与相机距离嵌）所以**不能直接拿 camera.nearPlane/farPlane**。
	float nearPlane = 0.05f;
	float farPlane = 1000.0f;
};

}  // namespace RGL
