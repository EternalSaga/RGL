module;

#include <algorithm>      // std::ranges::all_of / sort（Lua 表键形态判定）
#include <cmath>          // std::floor / std::fabs（整值归一）
#include <filesystem>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <fmt/format.h>

export module schema_loader;

import rlstd;

namespace RGL {

export struct SchemaField {
	std::string name;
	std::string type;
	int rowCount = 0;
	int columnCount = 0;
};

export struct SchemaPushConst {
	std::string name;
	std::vector<SchemaField> fields;
};

export struct SchemaDescSlot {
	std::string name;
	int index = -1;
	std::string shape;
	int heapSlotOffset = -1;
	std::string cppName;
};

// 顶点属性：name + 类型 + 字节偏移。
// 顶点契约的 source of truth 是 schema 声明（slangc 不反射顶点 buffer 结构体，
// 见 git:phase2_plan.md Step 0c）：顶点数组是平铺字节流，shader 按 schema 偏移
// 用 ByteAddressBuffer 逐字段读取，不依赖任何 Slang struct 的内存布局。
export struct SchemaVertexAttr {
	std::string name;
	std::string type;  // packed C++/glm 类型名："float"/"vec2"/"vec3"/"vec4"/"ivec2"/"ivec3"/"ivec4"
	uint32_t offset = 0;
};

export struct SchemaVertexLayout {
	std::string name;  // 生成的 C++ 结构体名（如 "ModelVertex"）
	uint32_t stride = 0;  // 每顶点字节数（= 所有属性 packed 尺寸之和）
	std::vector<SchemaVertexAttr> attributes;
};

// 契约常量（无反射端对应，schema 声明是 source of truth）：
// 生成 .cppm 的 inline constexpr；shader 端字面量必须与 value 一致（靠 shader 头注释指向本值）。
export struct SchemaConstant {
	std::string name;
	uint32_t value = 0;
};

export struct Schema {
	std::vector<SchemaPushConst> pushConstants;
	std::vector<SchemaDescSlot> descriptorSlots;
	std::vector<SchemaVertexLayout> vertexLayouts;
	std::vector<SchemaConstant> constants;
};

// 定长数组形态 "T[N]" 的拆解（★1-a2-i：cullPush.planes 在 schema 里写 "vec4[6]"）。
// 成功 = 填出元素类型名与元素数；N 必须是**无溢出**的十进制正整数。非数组形态、缺 `]`、
// 空/非数字/为 0 的 N 一律 false，调用方按「不可映射」报错——🔴 不静默退化成标量类型
// （退化的后果是生成的 C++ 字段比 shader 少 N−1 个元素，而布局 static_assert 照样过）。
// 刻意**不递归**解析多维数组：本项目 slang 侧零使用，递归只会把「写错形态」变成「看着能过」。
inline bool splitArrayAttrType(std::string_view type, std::string_view& outElem, uint32_t& outCount) {
	const size_t open = type.find('[');
	if (open == std::string_view::npos || type.size() < 4 || type.back() != ']')
		return false;
	const std::string_view digits = type.substr(open + 1, type.size() - open - 2);
	if (digits.empty() || digits.size() > 9)  // >9 位不可能是合法数组长度（按溢出防护砍掉）
		return false;
	uint32_t count = 0;
	for (const char ch : digits) {
		if (ch < '0' || ch > '9')
			return false;
		count = count * 10u + static_cast<uint32_t>(ch - '0');
	}
	if (count == 0)
		return false;
	outElem = type.substr(0, open);
	outCount = count;
	return true;
}

// 顶点属性类型的 packed 字节尺寸（C++/glm 约定：vec3=12B 等）。
// 未知类型返回 0，由校验方报错。
export inline uint32_t vertexAttrSize(std::string_view type) {
	if (type == "float") return 4;
	if (type == "vec2") return 8;
	if (type == "vec3") return 12;
	if (type == "vec4") return 16;
	if (type == "ivec2") return 8;
	if (type == "ivec3") return 12;
	if (type == "ivec4") return 16;
	// Phase 10 批 10.2a：整型标量属性（ObjectDraw.totalTriangles/vertexOffset）
	if (type == "int") return 4;
	if (type == "uint") return 4;
	// 定长数组形态 "T[N]"（★1-a2-i）：元素尺寸 × N，元素名仍走上面那张表
	std::string_view elem;
	uint32_t count = 0;
	if (splitArrayAttrType(type, elem, count)) {
		const uint32_t elemSize = vertexAttrSize(elem);
		return elemSize == 0 ? 0 : elemSize * count;
	}
	return 0;
}

// 标量/向量形态的映射表（"T[N]" 的**元素名**也走这张表，见 vertexAttrCppType）。
// 未知类型返回 nullptr。
inline const char* vertexAttrScalarCppType(std::string_view type) {
	if (type == "float") return "float";
	if (type == "vec2") return "glm::vec2";
	if (type == "vec3") return "glm::vec3";
	if (type == "vec4") return "glm::vec4";
	if (type == "ivec2") return "glm::ivec2";
	if (type == "ivec3") return "glm::ivec3";
	if (type == "ivec4") return "glm::ivec4";
	// Phase 10 批 10.2a：与 slang_reflect typeToCppType 的标量名对齐
	if (type == "int") return "int32_t";
	if (type == "uint") return "uint32_t";
	return nullptr;
}

// 顶点属性类型 → 生成代码中的 C++ 类型（未知/不可映射类型返回**空串**，由校验方报错）。
// 三种形态：标量/向量名（"float"/"vec4"/"uint"…）、定长数组 "T[N]"（★1-a2-i 的
// cullPush.planes 写 "vec4[6]"）、**嵌套契约 struct**（批 R §9.4：矩阵惯用法 ObjectMatrix
// 与灯数组 GpuLight 要作为字段嵌进别的契约 struct，schema 里直接写 struct 名，
// 数组形态写 "GpuLight[8]"）。
// 🔴 嵌套名字的合法判据**定死**：只有出现在本次 schema 的 vertexLayouts 名单里
// （contractStructs，调用方从 schema.vertexLayouts 构造）才算合法嵌套类型，否则仍返回
// 空串——拼错的名字必须失败，不能"看起来像个标识符就放行"。默认空集 = 调用方没传名单时
// 嵌套名一律不可映射（失败方向）。
// 数组分支产出的字符串与 slang_reflect.cppm::typeToCppType 的 Kind::Array 分支**逐字符
// 相同**——validateSlangVertexLayout 比的就是这两个字符串，两边各写一份格式 = 迟早漂，
// 改任一侧必须同步另一侧。
export inline std::string vertexAttrCppType(std::string_view type,
	const std::set<std::string>& contractStructs = {}) {
	if (const char* scalar = vertexAttrScalarCppType(type))
		return scalar;
	if (contractStructs.contains(std::string(type)))
		return std::string(type);
	std::string_view elem;
	uint32_t count = 0;
	if (!splitArrayAttrType(type, elem, count))
		return {};
	if (const char* elemType = vertexAttrScalarCppType(elem))
		return fmt::format("std::array<{}, {}>", elemType, count);
	if (contractStructs.contains(std::string(elem)))
		return fmt::format("std::array<{}, {}>", elem, count);
	return {};
}

export inline bool has(const nlohmann::json& obj, std::string_view key) {
	return obj.contains(key);
}

export inline const char* getString(const nlohmann::json& obj, std::string_view key) {
	if (!has(obj, key)) return nullptr;
	return obj[key].get_ref<const std::string&>().c_str();
}

// ─── Lua → nlohmann::json 通用转换（reflection_tool render 的 --lua 入口）──────────
//
// 为什么要有它：`render` 子命令只吃 JSON，而项目的 schema 一律是 Lua（*_schema.lua /
// configs/*.lua，用户拍板「全局风格统一」）。本函数是那两处扩展里的第一处——**扩既有件**，
// 不是新造机制：沙箱、读取路径、报错出口全部照抄下面的 loadSchema。
//
// 🔴 沙箱不是防攻击（schema 是仓库内文件），是让「在 schema 里写函数 / 读环境」这种形态
// **当场报错而不是静默生效**：dofile/loadfile/require/package 置 nil 挡住"读别的文件"，
// 而函数/userdata 值本身转不出 JSON ⟹ 下面必须**抛**，绝不许转成 null 或空串（静默降级
// 会让"schema 里写了个函数"变成"该字段读起来是空的"，那是最难归因的一类错）。
//
// 数字：Lua 5.4 区分整数与浮点，但 sol2 的 `get_type()` 两者都报 `number`。这里按
// 「值本身是整值」归一为整数（timeout_ms = 2000 必须渲染成 `2000` 而不是 `2000.0`，
// 否则模板产出的 C++ 字面量带小数点、`uint32_t` 字段当场编不过）。
//
// 表：键全为 1..n 连续整数 → JSON 数组；全为字符串 → JSON 对象；混合/非连续/其它键类型
// → 抛（JSON 表达不了，静默挑一种 = 给下游一份看起来正常的错形状）。

/** 递归深度上限（自引用表的最便宜防线；schema 实际深度 ≤4，留足余量）。 */
inline constexpr int kLuaJsonMaxDepth = 32;

export nlohmann::json luaValueToJson(const sol::object& value, std::string_view where, int depth);

/** 一个 Lua 表的键值对（显式类型而不是 `auto` 形参：泛型 lambda 里 `.as<T>()` 要写 `template as`）。 */
using LuaEntry = std::pair<sol::object, sol::object>;

/** 一个 Lua 表 → 数组或对象（判据见 luaValueToJson 的注释）。 */
inline nlohmann::json luaTableToJson(sol::table table, std::string_view where, int depth) {
	if (depth > kLuaJsonMaxDepth)
		throw std::runtime_error(fmt::format(
			"{}: 嵌套深度超过 {}——自引用表或写错了层级", where, kLuaJsonMaxDepth));

	std::vector<LuaEntry> items;
	items.reserve(static_cast<size_t>(table.size()));
	table.for_each([&items](const sol::object& key, const sol::object& val) {
		items.emplace_back(key, val);
	});
	if (items.empty())
		return nlohmann::json::object();  // 空表按对象出（nlohmann 的默认形态）

	const bool allInteger = std::ranges::all_of(items, [](const LuaEntry& kv) {
		return kv.first.get_type() == sol::type::number
		    && kv.first.as<double>() == std::floor(kv.first.as<double>());
	});
	const bool allString = std::ranges::all_of(items, [](const LuaEntry& kv) {
		return kv.first.get_type() == sol::type::string;
	});
	if (!allInteger && !allString)
		throw std::runtime_error(fmt::format(
			"{}: 表里混了不同形态的键（整数键与字符串键不能同表——JSON 表达不了）", where));

	if (allString) {
		nlohmann::json out = nlohmann::json::object();
		for (const auto& [key, val] : items)
			out[key.as<std::string>()] =
				luaValueToJson(val, fmt::format("{}[{}]", where, key.as<std::string>()), depth + 1);
		return out;
	}

	std::vector<double> keys;
	keys.reserve(items.size());
	for (const auto& [key, _val] : items)
		keys.push_back(key.as<double>());
	std::ranges::sort(keys);
	for (size_t i = 0; i < keys.size(); ++i) {
		if (keys[i] != static_cast<double>(i + 1))
			throw std::runtime_error(fmt::format(
				"{}: 整数键必须是 1..n 连续（第 {} 个键是 {:g}）——"
				"稀疏数组/0 基下标在 JSON 里没有对应形态",
				where, i + 1, keys[i]));
	}
	nlohmann::json out = nlohmann::json::array();
	for (const auto& [key, val] : items)
		out[static_cast<size_t>(key.as<double>()) - 1] = luaValueToJson(
			val, fmt::format("{}[{:g}]", where, key.as<double>()), depth + 1);
	return out;
}

export nlohmann::json luaValueToJson(const sol::object& value, std::string_view where, int depth) {
	switch (value.get_type()) {
	case sol::type::lua_nil: return nullptr;
	case sol::type::boolean: return value.as<bool>();
	case sol::type::number: {
		const double num = value.as<double>();
		if (num == std::floor(num) && std::abs(num) < 9.0e15)
			return static_cast<int64_t>(num);  // 整值归一（见函数头注释）
		return num;
	}
	case sol::type::string: return value.as<std::string>();
	case sol::type::table: return luaTableToJson(value.as<sol::table>(), where, depth);
	// 🔴 这四类一律抛：转成 null/空串 = 「schema 里写了函数」静默变成「该字段是空的」
	case sol::type::function:
		throw std::runtime_error(fmt::format(
			"{}: schema 里写了函数值——能力清单是数据，不是代码（沙箱纪律：函数不落地）", where));
	case sol::type::userdata:
	case sol::type::lightuserdata:
		throw std::runtime_error(fmt::format("{}: schema 里写了 userdata 值", where));
	case sol::type::thread:
		throw std::runtime_error(fmt::format("{}: schema 里写了协程值", where));
	default:
		throw std::runtime_error(fmt::format("{}: 无法转成 JSON 的 Lua 值", where));
	}
}

/**
 * 读一份 Lua 文件并把它的返回值转成 `nlohmann::json`（`render --lua` 的输入面）。
 * @param luaPath 相对仓库根的 Lua 文件路径
 * @return 根表的 JSON 形态
 * @throw std::runtime_error 读不到 / 根不是表 / 含函数等不可转换值 / 键形态非法
 */
export nlohmann::json loadLuaToJson(const std::filesystem::path& luaPath) {
	sol::state lua;
	lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
	lua["dofile"] = sol::nil;
	lua["loadfile"] = sol::nil;
	lua["package"] = sol::nil;
	lua["require"] = sol::nil;

	const std::string path = luaPath.string();
	auto result = lua.do_file(path.c_str());
	if (!result.valid()) {
		const sol::error err = result;
		throw std::runtime_error(fmt::format("Failed to load Lua: {}: {}", path, err.what()));
	}
	if (result.return_count() < 1)
		throw std::runtime_error(fmt::format("{}: 没有返回值（schema 文件必须 `return {{ … }}`）", path));
	const sol::object root = result.get<sol::object>(0);  // 0 = 第一个返回值（sol2 的偏移口径）
	if (root.get_type() != sol::type::table)
		throw std::runtime_error(fmt::format(
			"{}: 根不是表（实际是 {}）——schema 文件必须 `return {{ … }}`", path,
			static_cast<int>(root.get_type())));
	return luaTableToJson(root.as<sol::table>(), path, 0);
}

export Schema loadSchema(const std::filesystem::path& luaPath) {
	sol::state lua;
	lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
	lua["dofile"] = sol::nil;
	lua["loadfile"] = sol::nil;
	lua["package"] = sol::nil;
	lua["require"] = sol::nil;

	auto result = lua.do_file(luaPath.string().c_str());
	if (!result.valid()) {
		throw std::runtime_error(fmt::format("Failed to load schema: {}", luaPath.string()));
	}

	sol::table rootTable = result[0].as<sol::table>();

	Schema schema;

	sol::table pcArray = rootTable["pushConstants"];
	if (pcArray.valid() && pcArray.is<sol::table>()) {
		for (int i = 1; i <= static_cast<int>(pcArray.size()); ++i) {
			SchemaPushConst pc;
			sol::table pcItem = pcArray[i];
			sol::optional<std::string> nameOpt = pcItem["name"];
			if (nameOpt) pc.name = *nameOpt;

			sol::table fieldsArray = pcItem["fields"];
			if (fieldsArray.valid() && fieldsArray.is<sol::table>()) {
				for (int fi = 1; fi <= static_cast<int>(fieldsArray.size()); ++fi) {
					SchemaField f;
					sol::table fObj = fieldsArray[fi];
					sol::optional<std::string> fname = fObj["name"];
					if (fname) f.name = *fname;
					sol::optional<std::string> ftype = fObj["type"];
					if (ftype) f.type = *ftype;
					sol::optional<int> rc = fObj["rowCount"];
					if (rc) f.rowCount = *rc;
					sol::optional<int> cc = fObj["columnCount"];
					if (cc) f.columnCount = *cc;
					pc.fields.push_back(std::move(f));
				}
			}
			schema.pushConstants.push_back(std::move(pc));
		}
	}

	sol::table dsArray = rootTable["descriptorSlots"];
	if (dsArray.valid() && dsArray.is<sol::table>()) {
		for (int i = 1; i <= static_cast<int>(dsArray.size()); ++i) {
			SchemaDescSlot ds;
			sol::table dsItem = dsArray[i];
			sol::optional<std::string> nameOpt = dsItem["name"];
			if (nameOpt) ds.name = *nameOpt;
			sol::optional<int> idx = dsItem["index"];
			if (idx) ds.index = *idx;
			sol::optional<std::string> shape = dsItem["shape"];
			if (shape) ds.shape = *shape;
			sol::optional<int> hs = dsItem["heapSlotOffset"];
			if (hs) ds.heapSlotOffset = *hs;
			sol::optional<std::string> cppName = dsItem["cppName"];
			if (cppName) ds.cppName = *cppName;
			schema.descriptorSlots.push_back(std::move(ds));
		}
	}

	sol::table vlArray = rootTable["vertexLayouts"];
	if (vlArray.valid() && vlArray.is<sol::table>()) {
		for (int i = 1; i <= static_cast<int>(vlArray.size()); ++i) {
			SchemaVertexLayout vl;
			sol::table vlItem = vlArray[i];
			sol::optional<std::string> nameOpt = vlItem["name"];
			if (nameOpt) vl.name = *nameOpt;
			sol::optional<int> stride = vlItem["stride"];
			if (stride) vl.stride = static_cast<uint32_t>(*stride);
			sol::table attrsArray = vlItem["attributes"];
			if (attrsArray.valid() && attrsArray.is<sol::table>()) {
				for (int ai = 1; ai <= static_cast<int>(attrsArray.size()); ++ai) {
					SchemaVertexAttr attr;
					sol::table attrObj = attrsArray[ai];
					sol::optional<std::string> aname = attrObj["name"];
					if (aname) attr.name = *aname;
					sol::optional<std::string> atype = attrObj["type"];
					if (atype) attr.type = *atype;
					sol::optional<int> aoff = attrObj["offset"];
					if (aoff) attr.offset = static_cast<uint32_t>(*aoff);
					vl.attributes.push_back(std::move(attr));
				}
			}
			schema.vertexLayouts.push_back(std::move(vl));
		}
	}

	sol::table ctArray = rootTable["constants"];
	if (ctArray.valid() && ctArray.is<sol::table>()) {
		for (int i = 1; i <= static_cast<int>(ctArray.size()); ++i) {
			SchemaConstant ct;
			sol::table ctItem = ctArray[i];
			sol::optional<std::string> ctName = ctItem["name"];
			if (ctName) ct.name = *ctName;
			sol::optional<int> ctValue = ctItem["value"];
			if (ctValue) ct.value = static_cast<uint32_t>(*ctValue);
			schema.constants.push_back(std::move(ct));
		}
	}

	return schema;
}

}  // namespace RGL
