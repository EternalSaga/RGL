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
 * index 0 = 主光（写 scene data @0 头单灯）；deferred light pass 按 index
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

/** 前置声明：4 参版定义在 2 参版之后（2 参版委派给它，故需先见声明）。 */
export inline Bounds frustumBoundingSphere(const glm::mat4& view, const glm::mat4& proj,
	float nearPlane, float farPlane);

/**
 * 由 proj **反解** near/far 求视锥精确外接球（闭式解）。
 *
 * 视锥 8 角共球、球心在视轴上——结果只依赖 near/far/fov/aspect，**与相机姿态
 * （yaw/pitch/位置）完全无关**，这是阴影稳定性的关键：半径随相机连续变化 =
 * 光空间投影逐帧缩放 = 影子在地面上"游动"（2026-09-29 用户实测：机位平移 0.06，
 * 井盖阴影时有时无）。8 角都在球面上 → 这就是最小外接球，保证**整个视锥在球内**。
 *
 * ⚠️ **精度契约弱于 4 参重载**（大 near/far 比下反解 far 不可靠，见函数体注释）。
 * 新代码请直接用 4 参版。
 *
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
	// **前两版的教训**（→ git:调试历史 5a-5/5a-6）：Eberly 增量法与 extractFrustum
	// 平面符号约定混用 → 球装不下视锥；改 8 角世界 AABB 法修好正确性但引入姿态依赖
	// （AABB 随 yaw 变，实测还比精确球大 16%）。闭式解既不碰符号约定也不碰姿态。
	// **精度（2026-09-30 批 12.1 单测抓出，见 tests/test_shadow_geometry
	// 「frustumBoundingSphere 覆盖性」）**：本 2 参版**从矩阵反解** near/far，在大
	// near/far 比下不可靠。`far = B/(1+A)` 里 `1+A` 是两个 ~1 的数相减，而**相消
	// 发生在 glm::perspective 把 A 存进 float32 矩阵的那一刻**：A ≈ −1 − 2n/f，
	// float32 在 1.0 附近的间隔 1.19e-7 ⟹ 1+A 的相对误差 ≈ 1.19e-7·f/(2n)，被比值
	// 线性放大。实测 near/far=8e4 时反解 far 偏小 0.4%，超过下面 ×1.001 的保守余量
	// ⟹ 球装不下真视锥（欠覆盖 0.49%）。
	//
	// 下面把反解放 double **修不了**它（信息在更上游就没了），只把"矩阵自身舍入
	// 之后"的残余挪到 double。**真解是别反解**——见 4 参重载：调用方本来就有真值。
	// 保留本 2 参版是因为它语义自洽（只吃矩阵），但**精度契约按 4 参版定**。
	const double A = proj[2][2];
	const double B = proj[3][2];
	// 有效透视（RH_ZO，f > n > 0）：w 行 = (0,0,−1,0)、A = f/(n−f) ∈ (−∞,−1)、B = A·n < 0。
	// **踩过的坑**（5a-7 首版）：把 `A < −1` 当退化条件写——恰好把所有正常透视判成退化，
	// 半径恒 0 → 全体积单位阵 → 小物件阴影全灭（恒等阵伪影还姿态无关，把对拍指标全骗过）。
	// 正确退化 = 正交（w 行非 −1）或反推 near/far 不成序（含 A=−1 除零 → far=∞ 被序判挡下）。
	if (proj[2][3] != -1.0f || B >= 0.0 || A >= 0.0)
		return Bounds{glm::vec3{0.0f}, 0.0f};
	const double nearPlane = B / A;
	const double farPlane = B / (1.0 + A);
	if (!(farPlane > nearPlane) || !(nearPlane > 0.0))
		return Bounds{glm::vec3{0.0f}, 0.0f};
	return frustumBoundingSphere(view, proj, static_cast<float>(nearPlane),
		static_cast<float>(farPlane));
}

/**
 * 由**显式** near/far 求视锥精确外接球（**首选入口**，2026-09-30 批 12.1 新增）。
 *
 * ## 为什么要有这个重载
 *
 * 见 2 参版的精度说明：矩阵反解 far 的误差被 near/far 比线性放大（float32 RH_ZO
 * 透视矩阵在比值 ≳1e3 时就编不准 far，≳1e4 时误差超过保守余量）。而**调用方本来
 * 就有真值**——级联切片用 `projWithDepthRange(proj, nearC, farC)` 造子矩阵，
 * `nearC/farC` 就在同一行代码里（render_list_impl 的级联循环）。
 *
 * 这与 `projWithDepthRange` 的既有理由一字不差（"直接拿字段不会漂；反推需要一组
 * 奇性算术，且不带来任何额外精度"）——本项目早就在那条路上走了，唯独
 * `frustumBoundingSphere` 还在反解。
 *
 * @param view 相机 view（球心视空间 → 世界用其逆）
 * @param proj 投影矩阵（**只取 fov/aspect 两行 + 透视性判定**，不用于反解 near/far）
 * @param nearPlane 近平面距离（真值）
 * @param farPlane 远平面距离（真值）
 * @return {center, radius}；非透视 proj 或 near/far 不成序返回半径 0
 */
export inline Bounds frustumBoundingSphere(const glm::mat4& view, const glm::mat4& proj,
	float nearPlane, float farPlane) {
	// 透视性判定仍看矩阵的 w 行（proj 的固有性质，与 near/far 反解无关）
	if (proj[2][3] != -1.0f)
		return Bounds{glm::vec3{0.0f}, 0.0f};
	if (!(farPlane > nearPlane) || !(nearPlane > 0.0f))
		return Bounds{glm::vec3{0.0f}, 0.0f};
	const double n = nearPlane, f = farPlane;
	const double tanHalf = 1.0 / static_cast<double>(proj[1][1]);
	const double aspect = static_cast<double>(proj[1][1]) / static_cast<double>(proj[0][0]);
	const double cornerScale = tanHalf * std::sqrt(aspect * aspect + 1.0);  // 角点离轴距/深度
	const double rn = n * cornerScale;
	const double rf = f * cornerScale;
	const double zc = (f * f - n * n + rf * rf - rn * rn) / (2.0 * (f - n));
	// ×1.001 余量：留给 float32 下"8 角距 = r"的舍入竞争（探针 ~1e-5 量级）；
	// 阴影侧的保守方向是宁大勿小（大 = 多收投影体，小 = 漏画）。
	// near/far 已是真值，这个余量不必再承担"盖反解相消误差"的职责。
	const float radius = static_cast<float>(std::sqrt((f - zc) * (f - zc) + rf * rf) * 1.001);
	const glm::vec4 center = glm::inverse(view) * glm::vec4(0.0f, 0.0f, static_cast<float>(-zc), 1.0f);
	return Bounds{glm::vec3{center} / center.w, radius};
}

/**
 * 阴影级联切分用的距离量化（向上取整到 **2 的幂**）。
 *
 * 为什么要量化：自适应 near/far 随相机连续变（`projectionSystem` 按相机到场景球心的距离
 * 推导），而级联盒半径是它的**闭式函数** ⟹ 相机一动，整套纹素格就跟着缩放 = 整张阴影图
 * 重新量化到一套新格点。
 *
 * 为什么从 1/8 格改成 2 的幂（2026-10-19 债-139 实测换的）。跳变的可见代价取决于**单次跨
 * 档的纹素变化比例**，而用户看到的是**每米行走累计跳多少**。1/8 格在 far≈1.7 处每档只差
 * 7%，看着温和，代价是远界每走 **0.136m** 就跨一档。实测（同机位把量化远界钉到相邻档：
 * 相机逐位不动 ⟹ 零视差、逐位精确对应，是本债唯一没有测量地板的仪器）：
 *   每跨一档固定付出 **0.32–0.40% 像素阴影状态翻转**、**1.0–1.3% 像素亮度变化 >0.02 线性域**，
 *   且**与跨多大无关**（1.625→1.750 与 2.500→3.000 同量级）⟹ 是格点重新量化，不是精度累积。
 * ⟹ 细格 = 高频小跳 = 用户报的"飞相机时影子一跳一跳"。2 的幂把**相邻跨档点的平均间距**从
 * 0.136m 拉到 1.9m（本机位实测 d(far)/d(相机位移) = 0.0513/0.056 = 0.916/m，旧格距 0.125，
 * 新格距 = far 自身），单次幅度换成 2 倍纹素，按"每米行走累计跳多少"算净跳动降约 14 倍。
 * ⚠ 但**别把它读成"从此每 1.9m 才跳一次"**：1.9m 是跨档点之间的平均间距，而本机位
 * far=1.7135 落在 [1,2) 档的**上沿**，往外走 0.31m、往里走 0.78m 就会跨档（旧格距下分别是
 * 0.04m 与 0.09m）。改善倍数随相位在 2.3×–8× 之间，14× 是均值口径。
 * 另白赚两条：① 纹素尺寸随之成为 2 的幂 ⟹ **相邻档的纹素格互相嵌套**（粗格线必落在细格线
 * 上），跨档时影边最多挪半个粗纹素，而不是像 1/8 格那样两套互不相关的格点随机错位；
 * ② 量化因此**幂等**（`q(q(d)) == q(d)`，恰为 2 的幂时取自身而非翻倍）。
 *
 * 方向仍是**向上取整**（宁大勿小 = 覆盖性不破）。CPU 建切片与 shader 选级阈值必须同用
 * 量化值（`render_list_impl` 两处同源调用），否则选级与内容错位。
 */
export inline float quantizeShadowDistance(float d) {
	if (!(d > 0.0f) || !std::isfinite(d))
		return d;  // 0 / 负 / 非有限原样返回（旧实现对这三类同样不产生可用格点）
	int exponent = 0;
	const float mantissa = std::frexp(d, &exponent);  // d = mantissa·2^exponent，mantissa ∈ [0.5,1)
	// mantissa == 0.5 ⟺ d 恰为 2 的幂（d = 2^(exponent-1)）⟹ 取它自身，
	// 否则（d ∈ (2^(e-1), 2^e)）取上界 2^exponent。
	return std::ldexp(1.0f, mantissa == 0.5f ? exponent - 1 : exponent);
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
 *
 * **texel 吸附是世界锚定的**（5a-8b 重写）：把切片球心在固定光基
 * （s = d×up 归一、u = s×d、d = 光向——三个轴只依赖太阳方向，与世界原点一样恒定）下的
 * 三坐标各 round 到 `2r/mapSize` 格点，再建盒。此前版本吸附的是"中心在自己盒里的 NDC"
 * ——恒等于 (0,0)——等于没吸：盒跟着视锥球心走，**相机原地转视角**时球心
 * （= eye + forward·zc）绕 eye 划半径 zc 的弧，一次扫过几十个 texel → 纹素格整体
 * 在世界里滑 → 影子随视角游动（5a-8 只修掉了平移分量，旋转分量被这条恒等吸附掩盖）。
 * 三轴全吸（含光轴 z）：z 不吸则 eyeOffset/tMin/tMax 连滑 → 深度量化格漂 → 自阴影
 * 边缘同一步伐呼吸。吸附后体参数只经 `c'` 依赖姿态，而 `c'` 在格内恒定 → 完全恒定。
 *
 * @param light 主光（取 direction 定光空间基；只取方向，位置由 eyeOffset 定）
 * @param slice 该级子视锥的包围球（定 XY 半宽与受光面深度范围 [−r, r]）
 * @param casterMin 全部投影体世界 AABB 的最小角（定 Z 下界）
 * @param casterMax 全部投影体世界 AABB 的最大角（定 Z 上界）
 * @param mapSize 该级图块边长（texel 吸附格距 = 2r/mapSize）
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
	// 世界锚定的光空间基（与 glm::lookAt(eye, eye+d, up) 的行向量基同序）：
	const glm::vec3 s = glm::normalize(glm::cross(d, up));
	const glm::vec3 u = glm::cross(s, d);
	const float texel = 2.0f * r / static_cast<float>(mapSize);
	// 三轴 round 到格点，位移 ≤ 半 texel（±pad r·0.02 远大于它，覆盖性不破）。
	const glm::vec3 snappedCenter = slice.center + s * (std::round(glm::dot(slice.center, s) / texel) * texel - glm::dot(slice.center, s))
		+ u * (std::round(glm::dot(slice.center, u) / texel) * texel - glm::dot(slice.center, u))
		+ d * (std::round(glm::dot(slice.center, d) / texel) * texel - glm::dot(slice.center, d));
	// 沿光轴取并：受光面占 [−r, r]（切片球），投影体占其 AABB 在光轴上的投影。
	// pad 一点余量，避免投影体正好压在近/远平面上被裁掉。
	// **踩过的坑**（5a-8b）：把"世界 AABB 最小角"当光轴最远点——`dot(casterMin, d)` 取到
	// 最小值要求 d 分量全正；太阳 d.y<0 时真正的极值角是 (x 按符号取 min/max, **y 取
	// max**, z 按符号取 min/max)（support function）。用错角的后果：tMin 恒被 −r 支配
	// （实测 eyeOffset 恒 = r·1.02），比切片球顶高的建筑整批被近平面裁掉不画 → 它罩到的
	// 那块地面"有没有影子"取决于受光片元落在哪一级（各级 −r 不同），旋转视角换级 →
	// 影子忽明忽暗（= 用户报的"游动"）。
	const float pad = r * 0.02f;
	const glm::vec3 casterFar(d.x >= 0.0f ? casterMin.x : casterMax.x,
		d.y >= 0.0f ? casterMin.y : casterMax.y,
		d.z >= 0.0f ? casterMin.z : casterMax.z);
	const glm::vec3 casterNear(d.x >= 0.0f ? casterMax.x : casterMin.x,
		d.y >= 0.0f ? casterMax.y : casterMin.y,
		d.z >= 0.0f ? casterMax.z : casterMin.z);
	const float tMin = std::min(-r, glm::dot(casterFar - snappedCenter, d)) - pad;
	const float tMax = std::max(r, glm::dot(casterNear - snappedCenter, d)) + pad;
	const float eyeOffset = -tMin;
	const glm::mat4 view = glm::lookAt(snappedCenter - d * eyeOffset, snappedCenter, up);
	const glm::mat4 proj = glm::ortho(-r, r, -r, r, 0.0f, tMax - tMin);
	return ShadowVolume{proj * view, view, r, eyeOffset, tMin, tMax};
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
 * 写进 scene data 头 @48（SceneFrameData.envParams），pbr.slang readEnvParams 消费。
 * mipRange = cubemap mipLevels−1（上传期才知道，故 main 在 loadEnvCubeMapFiles 后填）；
 * 默认全 0 = 无环境（env 镜面/漫反射项黑、天穹关；deferred 的天穹总闸 = intensity，
 * docs/archive/图变体收敛_plan.md §6.2）。
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
	// ── 批 12.6 TAA 运行参数（configs/scene.lua `taa` 段，同 aoStrength 的搭车路）──
	// enabled 决定**图形态**（decorateTaaPass 是否调用，启动期读一次）+ 每帧是否抖动；
	// feedback/jitterScale/motionCompensation 每帧进 scene data @723584，imgui 可调。
	bool taaEnabled = false;
	float taaFeedback = 0.01f;   ///< 本帧权重（0.01 ⇒ 等效累积窗 ≈100 帧，扫档依据 → scene_config.cppm 同名字段）
	float taaJitterScale = 1.0f;  ///< 抖动幅度倍率（1 = ±0.5px）
	bool taaMotionCompensation = true;  ///< 是否用 prevSpin 补偿全场景旋转
	// 批 13.5 基空间累积（`taa.base_space`，语义于批 13.5 改义 → scene_config.cppm 同名字段
	// 注释）：true ⟹ 每帧把「本帧内容位移 c_t」写进 scene data @723584.zw、把「上一帧内容
	// 位移 c_{t−1}」写进 @723600，由 **taa.frag** 消费（输入侧 un-jitter + 历史对齐）。
	// 唯一消费者 = taa.frag（批 13.2 时代是 tonemap）；tonemap 与 taa_commit 不认识它。
	// false = 两区恒 0 = 逐位回到改前 `base_space = false` 形态（A/B 对照点）。
	// ⚠ 默认值与 SceneConfig 的**生效默认**对齐（true）。曾写 false 而 main 总从 config 覆盖
	//   ⟹ 直接构造 SceneEnv 的测试拿到 false、生产拿到 true = 谎言行（2026-10-04 清点发现）。
	bool taaBaseSpace = true;
	// 批 13.6 AO 施加点标志（scene data @723600.z）：true ⟹ AO 已由 **taa pass** 施加，
	// tonemap 跳过（否则双施）；false ⟹ taa pass 不在图里，tonemap 是唯一施加点。
	// ⚠ 取值 = **实际图形态**（`decorateTaaPass` 返回值，main 启动期回填），**不是**上面的
	//   `taaEnabled` 配置键：自定义 blit 上屏图配置开了 TAA 也会装饰失败，拿配置派生 = AO
	//   一次都不施加。也**不得**做成 imgui 滑杆（图形态启动期定死，运行期改会让标志与
	//   图脱钩 = 双施或零施）。
	bool aoAppliedPreTemporal = false;
	/**
	 * 批 14.5 第四批：**亚像素抖动**是否由 FSR 档要求（`runtime.fsr_jitter` 且 FSR 接起来了）。
	 *
	 * 与 `aoAppliedPreTemporal` 是**同一个病灶**：抖动此前也被硬编码成 TAA 专属
	 * （`env.taaEnabled` 一关，投影就不抖），而 FSR3-upscaler 是**抖动相机**时域升频器，
	 * 没有亚像素抖动的输入根本不在它的设计点上（§1.14 那条未结案假设的验证前提）。
	 * TAA 在图里时本字段**不参与决策**（走原路径，周期 16 / 幅度 taa_jitter_scale）
	 * ⟹ TAA 档逐位不变。
	 */
	bool fsrJitter = false;
	/// 抖动序列**周期**（= `taaJitterPixels` 的 period 实参），必须等于 vendor 内部自己算的
	/// 那个 `JitterSequenceLength()`，由 `fsrJitterPhaseCount(renderW, upscaleW)` 给出。
	/// 0 = 未设置，消费侧回落 kTaaJitterPeriod。字段名刻意不叫 phaseCount：与同名自由函数
	/// 并存时 `env.fsrJitterPhaseCount(...)` 这种表达式是纯阅读陷阱。
	uint32_t fsrJitterPeriod = 0;
	// FSR **申报侧**两枚（能力观测批 2：`/state` fsr 段的 gateOpen / temporalConfig）：
	// gateOpen = runtime.lua `aa == "fsr"` 门开没开；fsrTemporalConfig = `fsr.temporal`
	// 键原值。装配期由 assembleBackEnd 回填（与 fsrJitter 同批同时机）。**生效侧**
	// （usable / temporalEffective / context 实收尺寸）不住这里——那是 Renderer 的
	// 现状，两处各记各的，「申报了没生效」才有得查。
	bool fsrGateOpen = false;
	bool fsrTemporalConfig = false;
	// 重投影消融门（`taa.reprojection`，默认 true = 逐位今天）：false ⟹ taaSystem 把
	// 「上一帧矩阵」载荷换成本帧抖动后矩阵 + 位移清零 + motion 单位阵 ⟹ shader 的
	// prevUvBase ≡ baseUv（历史按同基位置采样）。判据消费方 = imgqa temporal-motion M3。
	bool taaReprojection = true;
	// 历史拒收诊断门（`taa.diag_reject`，默认关）：true ⟹ scene data @723600.w = 1，
	// taa.frag 把拒收码写进 taa_out.a（RGB 不动）。回读 = /shot attachment=taa_out。
	bool taaDiagReject = false;
	// 输入侧 un-jitter 的**重建核选择**（`taa.input_kernel`，批 13.8，默认 0 = 逐位今天）：
	// 0 = 双线性 / 1 = Catmull-Rom 4×4 / 2 = Lanczos3 6×6。scene data @723616.x，
	// 唯一消费者 = taa.frag `sampleInputColor`。判据消费方 = imgqa `--kernel`（temporal-flicker
	// / aa-coverage / temporal-feedback）。⚠ 非整数值按 `k<0.5` / `k<1.5` 落桶（不报错）。
	float taaInputKernel = 0.0f;
	// ── **历史裁剪消融门**（scene.lua `taa.clip_mode` / `taa.clip_soft`，批 13.11，默认 0 = 逐位今天）──
	// clip_mode：0 现行（box5 + 硬裁剪）/ 1 关盒 / 2 绕 centroid 扩 1.5× / 3 box9 / 4 软裁剪。
	// clip_soft（仅 mode=4）= **无量纲**时间常数：历史每帧最多走完「到盒边界距离」的 λ 分之一。
	//   ⚠ 选值安全线：鬼影衰减 ≈ 1/λ 帧 vs 累积窗 ≈ 1/α 帧——**λ < α 时盒不再是鬼影刹车**。
	// 为什么存在（根因：clip 在 blend 之前 ⟹ 一次裁剪 = 一次历史重置 ⟹ 相位依赖方差 100% 注入）
	// → `git:调试历史` 2026-10-06；档位全量语义 → `scene_data.cppm` TaaParamsBlock::clipControl。
	float taaClipMode = 0.0f;
	float taaClipSoft = 1.0f;
	// Phase 5.5 批次 4 天空盒（skybox / skyboxPipeline 两字段）已随 forward 绘制机制删除
	// （2026-10-18 图变体收敛刀②.2，docs/archive/图变体收敛_plan.md §6.2：deferred 的天穹对应物
	//  = env.intensity 总闸）。
};

/**
 * 场景整体时间旋转（registry context，main 启动期 emplace；animationSystem 每帧更新
 * angle/matrix）：speed=0 = 静止（configs/scene.lua 的 spin 开关）。
 * 现状承载"全场景同一旋转"（MVP = proj·view·matrix·world）；per-entity 旋转需求出现时
 * 改为写 Transform 组件，结构同构无需二次重构（ecs_compliance_review E4.1）。
 *
 * 帧驱动门（`spin_frame_angle`，默认 0 = 关，诊断激励专用；判据本体 =
 * frameDrivenSpinAngle 纯函数）：门开时 angle 是**帧号的纯函数**、与帧时长无关
 * ⟹ 角度序列跨运行可复现。dt 累积（speed·dt）每帧角度随帧时长漂，跨进程 A/B
 * （时域判据的参考图与候选图在两个引擎进程里渲染同一机位序列）无从对齐——
 * 理由全文写死在 tools/imgqa/temporal.cpp 头注释，本门就是它当年登记的
 * 「要做微动激励得先加按帧索引驱动的运动」那条新引擎面。
 */
export struct SceneSpin {
	glm::vec3 axis{0.5f, 1.0f, 0.0f};
	float speed = 1.0f;   // rad/s（dt 累积路径；门关态唯一路径）
	float angle = 0.0f;   // 累积角（animationSystem: angle += dt * speed）
	glm::mat4 matrix{1.0f};  // = rotate(axis, angle)，animationSystem 每帧更新
	// ── 帧驱动 spin（门 `spin_frame_angle`，默认 0 = 关；imgqa temporal-motion 诊断激励）──
	float frameAngle = 0.0f;  ///< rad/帧；≠0 = 门开：angle = frameAngle·frameIndex（dt 累积停用）
	uint32_t frameIndex = 0;  ///< animationSystem 每运行一次 +1（先加后用 ⟹ 首帧角 = frameAngle）
};

/**
 * 帧驱动 spin 角度（门 `spin_frame_angle` 的判据本体；提纯成自由函数的先例 =
 * taa_jitter.cppm——纯数学就该在 `just test` 里钉住）。
 *
 * angle = anglePerFrame · frameIndex——**无累积、无帧时长**：同一 frameIndex 在任何
 * 运行、任何机器上给出同一角度（跨进程 A/B 对齐的唯一前提）。float32 精度账：
 * frameIndex = 10⁴、anglePerFrame = 3.5e-3（0.2°/帧）时角 ≈35 rad，ULP ≈4e-6 rad，
 * 对应像素位移 869px 焦距 × 4e-6 ≈ 0.003px——远低于任何可测信号，无需 double。
 * @param anglePerFrame 每帧角增量（rad，可负 = 反向）
 * @param frameIndex 帧号（0 起；0 帧角 = 0）
 * @return 本帧角（rad）
 */
export float frameDrivenSpinAngle(float anglePerFrame, uint32_t frameIndex) {
	return anglePerFrame * static_cast<float>(frameIndex);
}

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
	// ── 批 12.6 TAA ──
	// `proj` = 本帧**实际渲染用**的投影（TAA 开时含亚像素抖动）。刻意抖在 proj 上：
	// 下游 objectMvps / skyInvViewProj(@64) / frustum 剔除 / picking / IZB 全部自动传染，
	// 与抖动后的深度缓冲天然自洽（AO/IZB 用 @64 重建世界坐标因此不需知道 jitter）。
	// `projUnjittered` = 未抖那份，供重投影把世界点投回**上一帧的像素中心**。
	// TAA 关：两者相同、jitterPx = 0（projectionSystem 根本不碰 proj ⇒ 逐位不变）。
	glm::mat4 projUnjittered{1.0f};
	glm::vec2 jitterPx{0.0f};  ///< 本帧抖动（像素单位，±0.5 档），进 scene data @723584.zw
};

/**
 * TAA 帧间状态（registry context，批 12.6；main 启动期 emplace，**taaSystem** 每帧
 * 先读后推进——它是本项目第一个跨帧 CPU 状态，与 GPU 侧 history 附件同生死）。
 *
 * 帧序（同一帧内）：① 用 prev* 算本帧 scene data 载荷 + 历史可用性 → ② 把本帧值写回
 * prev* 供下一帧。跳切/首帧/resize ⇒ `historyValid = false`（shader 走逐位旁路）。
 */
export struct TaaState {
	// ── 簿记（上一帧位姿，taaSystem 帧尾推进）──
	glm::mat4 prevViewProj{1.0f};  ///< 上一帧**抖动后** proj·view（与历史图内容同口径）
	glm::vec2 prevContentShift{};  ///< 上一帧**内容位移** c_{t−1}（像素；= 历史图自己那帧的位移）
	glm::mat4 prevSpin{1.0f};      ///< 上一帧 SceneSpin::matrix
	glm::vec3 prevCamPos{};
	glm::vec3 prevForward{0.0f, 0.0f, -1.0f};
	glm::ivec2 prevExtent{0, 0};   ///< resize 检测（RT 重建 ⇒ 历史作废）
	uint32_t frameIndex = 0;       ///< jitter 序列游标（跳切**不**重置：抖动相位连续无副作用）
	// ── 本帧载荷（taaSystem 先算、renderListSystem 后读；与簿记**分开存**：同一字段
	//    既当载荷又当簿记，就会把「谁先读谁后推进」写成隐式时序耦合）──
	glm::mat4 payloadPrevViewProj{1.0f};
	glm::vec2 payloadPrevContentShift{};  ///< 本帧载荷 = 上一帧内容位移（历史基空间修正用）
	glm::mat4 payloadMotion{1.0f};  ///< prevSpin·inverse(curSpin)（关补偿 = 单位阵）
	bool historyValid = false;      ///< 本帧历史可否复用（false = shader 输出本帧）
};

}  // namespace RGL
