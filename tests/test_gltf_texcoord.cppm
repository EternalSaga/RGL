module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <chrono>            // 临时文件唯一后缀（AGENTS GMF 例外清单成员，替代 pid，见 TempGlb 注释）
#include <filesystem>        // 临时 GLB 落盘（AGENTS GMF 例外清单成员，test_scene_config 同法）
#include <initializer_list>  // 花括号列表（modules 下不从 import 带进来）
#include <cmath>             // std::isfinite（自由函数不经 BMI 保证传递，GMF 自带）

// glTF TEXCOORD_0 步长回归（批 12.3d-C）——被测对象 = rgl/gltf_loader_impl.cppm
// expandPrimitive 的去索引展开读取（pos/nrm/uv 三个 accessor 流 + 去索引映射）。
//
// ## 钉住的历史 bug（docs/archive/issue_history.md 2026-08-30）
//
// cottage 偶发 artifact（区域固定黑斑 + Windows AV）：uv 属性流是 **VEC2**，展开时却按
// vec3 步长取（`uv[idx*3+…]`，应为 `uv[idx*2+…]`）→ 越界读 + 采样错位。"偶发 + 区域固定
// + 随构建时序变"的越界读签名查了 12+ 项才定位，而函数本体是纯字节搬运：
// **手构一个最小 GLB 在毫秒级就能钉死**。现码已正确（gltf_loader_impl.cppm L442），
// 此前三条 tests/ 里零覆盖。
//
// ## 夹具设计（为什么这套数据能让 vec3 步长变异必红）
//
//   · 6 源顶点 / 2 三角形，索引 {5,0,3, 1,5,4}：**乱序 + 跨三角形重复顶点**——
//     vertices[i] 必须对应 indices[i]（去索引展开语义），顺序展开写错也红；
//   · uv 逐分量互不相同（u∈{1,1.125,…} / v∈{100,103.5,…} 两族值不交叠），
//     idx*3 误读必然落在别的槽位 → 期望 != 误读值（测试内自带"步长模型可分辨性"
//     自检，见 fixture 可分辨性用例，不用改生产码就能验证夹具本身有分辨力）；
//   · 全部值二进制精确（1/8 的倍数），断言用**逐位相等**而非近似——loader 只做
//     memcpy + 赋值，任何重新计算/错位都不该出现。
//   · 指令建议的"z 槽哨兵 999.0"在 packed VEC2 布局下没有 z 槽可放（accessor 类型
//     就是 VEC2，塞第三分量即非法夹具），以"值域互异 + 乱序索引"达到同等检出率。
//
// ## 变异红线声明（改哪个 → 哪条红）
//
//   L442 `uv[idx*2+…]` → `uv[idx*3+…]`（历史原错）→ 「uv 展开」用例红（实测形态见
//   交付报告：idx*3 对末顶点越出 uv vector 尾部，读到的是堆上残留值，仍与期望不等）；
//   L440/L441 pos/nrm 的 idx*3 改成任何错位形态 → 「position/normal 展开」用例红；
//   去索引映射丢失（vertices[i] = src[i]）→ 两用例同时红（索引乱序就是为它准备的）。

export module test_gltf_texcoord;

import rlstd;
import test_support;
import gltf_loader;  // loadGltfModel / GltfModel / GltfPrimitive（rlbase）

namespace RGL {
namespace {

/* ────────────────────────── 夹具数据 ────────────────────────── */

constexpr int kVertexCount = 6;
/// 2 三角形：三角形 0 = (5,0,3)，三角形 1 = (1,5,4)。乱序 + 顶点 5 跨三角形复用，
/// 断言 vertices[i] ↔ indices[i] 的去索引映射。
constexpr std::array<uint32_t, 6> kIndices{{5, 0, 3, 1, 5, 4}};

// 全部取 1/8 的倍数 → float32 精确表示 → 断言可逐位比对。
glm::vec3 srcPos(int s) { return {1.5f * s + 0.25f, 3.0f * s - 1.5f, 7.0f - 2.5f * s}; }
glm::vec3 srcNormal(int s) { return {0.125f * s - 0.25f, 0.5f - 0.25f * s, 0.75f * s - 3.0f}; }
glm::vec2 srcUv(int s) { return {1.0f + 0.125f * s, 100.0f + 3.5f * s}; }

// 源顶点号 → 该顶点在**本测试展开期望表**里的位置（独立重算用，不走 loader）。
glm::vec3 expectPosAt(size_t expanded) { return srcPos(static_cast<int>(kIndices[expanded])); }
glm::vec3 expectNormalAt(size_t expanded) { return srcNormal(static_cast<int>(kIndices[expanded])); }
glm::vec2 expectUvAt(size_t expanded) { return srcUv(static_cast<int>(kIndices[expanded])); }

/* ────────────────────────── GLB 手构 ────────────────────────── */

// packed（bufferView 全部无 byteStride）布局常量；全部 4B 对齐（4 元组 float / uint32
// 天然满足 tinygltf 的 accessor 对齐校验，无需任何 padding 绕行）。
constexpr size_t kPosBytes = sizeof(float) * 3 * 6;  // 72
constexpr size_t kNrmBytes = sizeof(float) * 3 * 6;  // 72
constexpr size_t kUvBytes = sizeof(float) * 2 * 6;   // 48
constexpr size_t kIdxBytes = sizeof(uint32_t) * 6;   // 24
constexpr size_t kBinBytes = kPosBytes + kNrmBytes + kUvBytes + kIdxBytes;  // 216

void appendU32(std::string& out, uint32_t v) {
	for (int k = 0; k < 4; ++k)
		out.push_back(static_cast<char>((v >> (8 * k)) & 0xFF));  // 小端（glB 规范）
}
void appendF32(std::string& out, float f) {
	uint32_t u;
	std::memcpy(&u, &f, 4);
	appendU32(out, u);
}

/// 手构最小 GLB：12B 头 + JSON chunk（空格补 4 对齐）+ BIN chunk（零填充）。
/// 单 mesh 单 primitive 单 node 单 scene；accessor = POSITION VEC3 / NORMAL VEC3 /
/// TEXCOORD_0 VEC2 / indices SCALAR uint32。返回整个文件的字节串。
std::string buildGlb() {
	std::string bin;
	for (int s = 0; s < kVertexCount; ++s) {
		const glm::vec3 p = srcPos(s);
		for (int k = 0; k < 3; ++k) appendF32(bin, p[k]);
	}
	for (int s = 0; s < kVertexCount; ++s) {
		const glm::vec3 n = srcNormal(s);
		for (int k = 0; k < 3; ++k) appendF32(bin, n[k]);
	}
	for (int s = 0; s < kVertexCount; ++s) {
		const glm::vec2 u = srcUv(s);
		for (int k = 0; k < 2; ++k) appendF32(bin, u[k]);
	}
	for (uint32_t i : kIndices) appendU32(bin, i);

	std::string json;
	json += "{\"asset\":{\"version\":\"2.0\",\"generator\":\"rgl_tests_gltf_texcoord\"},\"scene\":0,";
	json += "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],";
	json += "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
	        "\"TEXCOORD_0\":2},\"indices\":3}]}],";
	json += "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}],\"bufferViews\":[";
	const size_t off[4] = {0, kPosBytes, kPosBytes + kNrmBytes, kPosBytes + kNrmBytes + kUvBytes};
	const size_t len[4] = {kPosBytes, kNrmBytes, kUvBytes, kIdxBytes};
	for (int v = 0; v < 4; ++v) {
		json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(off[v]) + ",\"byteLength\":" +
			std::to_string(len[v]) + (v == 3 ? "}" : "},");
	}
	json += "],\"accessors\":[";
	const char* type[4] = {"VEC3", "VEC3", "VEC2", "SCALAR"};
	const uint32_t comp[4] = {5126, 5126, 5126, 5125};  // FLOAT×3 + UNSIGNED_INT
	for (int a = 0; a < 4; ++a) {
		json += "{\"bufferView\":" + std::to_string(a) + ",\"componentType\":" +
			std::to_string(comp[a]) + ",\"count\":" + std::to_string(kVertexCount) +
			",\"type\":\"" + type[a] + "\"" + (a == 3 ? "}" : "},");
	}
	json += "]}";

	while (json.size() % 4 != 0) json.push_back(' ');    // JSON chunk：0x20 补（glB 规范）
	while (bin.size() % 4 != 0) bin.push_back('\0');     // BIN chunk：0x00 补

	std::string glb;
	appendU32(glb, 0x46546C67u);                                      // "glTF"
	appendU32(glb, 2u);                                               // version 2
	appendU32(glb, static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
	appendU32(glb, static_cast<uint32_t>(json.size()));
	appendU32(glb, 0x4E4F534Au);                                      // "JSON"
	glb += json;
	appendU32(glb, static_cast<uint32_t>(bin.size()));
	appendU32(glb, 0x004E4942u);                                      // "BIN\0"
	glb += bin;
	return glb;
}

/// 临时 GLB（构造写盘、析构删除）。命名后缀用 steady_clock 计数而非 pid：pid 需要
/// 平台头（unistd.h / process.h），测试双平台（Linux + Windows）故取 stdlib 等价物，
/// 目的相同 = 同机多次运行/并行实例互不踩踏。
class TempGlb {
  public:
	explicit TempGlb(std::string_view bytes)
	: path_(std::filesystem::temp_directory_path() /
			("rgl_test_gltf_" +
				std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
				".glb")) {
		FILE* f = std::fopen(path_.string().c_str(), "wb");  // 二进制写，Windows 不做换行翻译
		REQUIRE(f != nullptr);
		REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
		std::fclose(f);
	}
	TempGlb(const TempGlb&) = delete;
	TempGlb& operator=(const TempGlb&) = delete;
	~TempGlb() {
		std::error_code ec;
		std::filesystem::remove(path_, ec);
	}
	std::string pathStr() const { return path_.string(); }

  private:
	std::filesystem::path path_;
};

}  // namespace

TEST_SUITE("gltf_texcoord") {

TEST_CASE("GLB 夹具可分辨性：uv 值互异 + vec2/vec3 两种步长模型的读数必不同（夹具自检）") {
	// 夹具自身的阳性锚点：若 uv 数据设计得让 vec3 步长误读恰好撞上正确值，
	// 生产码变异也不会红——本用例不碰 loader，只验"夹具能分辨两种步长模型"。
	std::array<float, kUvBytes / sizeof(float)> uvFlat{};
	for (int s = 0; s < kVertexCount; ++s) {
		const glm::vec2 u = srcUv(s);
		uvFlat[s * 2 + 0] = u.x;
		uvFlat[s * 2 + 1] = u.y;
	}
	// 表尾用哨兵 999.0 填充代替真实越界（idx*3+1 最大 16 > 12）——只做两种步长
	// 模型的读数对比，不读真内存。
	std::array<float, 32> pad;
	for (auto& x : pad) x = 999.0f;
	for (size_t k = 0; k < uvFlat.size(); ++k) pad[k] = uvFlat[k];
	size_t differing = 0;
	for (size_t i = 0; i < kIndices.size(); ++i) {
		const uint32_t idx = kIndices[i];
		// idx=0 时两种步长读同一对 float（0·3 = 0·2），天然不可分辨——这是良构夹具
		// 的下限，不是缺陷；变异必红只依赖 idx≥1 的项 + "展开流含多个 idx≥1"。
		if (idx == 0)
			continue;
		const glm::vec2 misread(pad[idx * 3 + 0], pad[idx * 3 + 1]);
		const glm::vec2 want = expectUvAt(i);
		INFO("i=" << i << " idx=" << idx);
		CHECK((misread.x != want.x || misread.y != want.y));
		++differing;
	}
	// 展开流里 idx≥1 的项足够多（≥3）：主用例的逐顶点断言有足够分辨面。
	CHECK(differing >= 3);
}

TEST_CASE("GLB 解析成功：primitive/顶点/三角形计数 + hasUv + buffer 0 尺寸") {
	const TempGlb file(buildGlb());
	auto model = loadGltfModel(file.pathStr());
	REQUIRE(model.has_value());

	REQUIRE(model->primitives.size() == 1);
	const GltfPrimitive& p = model->primitives[0];
	CHECK(p.vertices.size() == kIndices.size());  // 去索引展开 = 索引数（非源顶点数）
	CHECK(p.triangleCount == 2);
	CHECK(p.hasUv == true);                       // TEXCOORD_0 在场 → D9 白纹理路径不触发
	CHECK(model->bufferData.size() == kBinBytes); // buffer 0 = BIN chunk 整体 move
	CHECK(model->images.empty());
	// 单根 node 无 TRS → 世界矩阵 identity。
	CHECK(p.nodeWorld == glm::mat4(1.0f));
}

TEST_CASE("uv 展开 = srcUv[indices[i]] 逐位相等（TEXCOORD_0 vec2 步长回归，2026-08-30 bug）") {
	const TempGlb file(buildGlb());
	auto model = loadGltfModel(file.pathStr());
	REQUIRE(model.has_value());
	const auto& v = model->primitives[0].vertices;
	REQUIRE(v.size() == kIndices.size());

	for (size_t i = 0; i < v.size(); ++i) {
		const glm::vec2 want = expectUvAt(i);
		INFO("expanded " << i << " (src " << kIndices[i] << ")");
		// 逐位相等：loader 只做 memcpy + 赋值，无算术，不该有任何误差。
		CHECK(v[i].uv.x == want.x);
		CHECK(v[i].uv.y == want.y);
	}
}

TEST_CASE("position/normal 展开 = src[indices[i]] 逐位相等（accessor 读取整体错位防御）") {
	const TempGlb file(buildGlb());
	auto model = loadGltfModel(file.pathStr());
	REQUIRE(model.has_value());
	const GltfPrimitive& prim = model->primitives[0];
	const auto& v = prim.vertices;
	REQUIRE(v.size() == kIndices.size());

	glm::vec3 centroid(0.0f);
	for (size_t i = 0; i < v.size(); ++i) {
		const glm::vec3 wantP = expectPosAt(i);
		const glm::vec3 wantN = expectNormalAt(i);
		INFO("expanded " << i << " (src " << kIndices[i] << ")");
		for (int k = 0; k < 3; ++k) {
			CHECK(v[i].position[k] == wantP[k]);
			CHECK(v[i].normal[k] == wantN[k]);
		}
		// 切线兜底链（fillTangents → frisvadTangent）不该产出 NaN——黑面 bug 的教训，
		// 顺带钉住"uv 步长错会传染手性/切线"这条放大路径。
		CHECK(std::isfinite(v[i].tangent.x));
		CHECK(std::isfinite(v[i].tangent.y));
		CHECK(std::isfinite(v[i].tangent.z));
		CHECK(std::isfinite(v[i].tangent.w));
		centroid += wantP;
	}

	// 包围球 = 展开顶点（enclosingSphere：质心 + 最远点距离，节点局部系）。
	centroid /= static_cast<float>(v.size());
	float radius = 0.0f;
	for (const auto& vert : v)
		radius = std::max(radius, glm::length(vert.position - centroid));
	testing::expectNear(prim.bounds.center.x, centroid.x, 1e-5);
	testing::expectNear(prim.bounds.center.y, centroid.y, 1e-5);
	testing::expectNear(prim.bounds.center.z, centroid.z, 1e-5);
	testing::expectNear(prim.bounds.radius, radius, 1e-5);
}

}  // TEST_SUITE("gltf_texcoord")

}  // namespace RGL
