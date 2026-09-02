module;

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
// 见 docs/archive/phase2_plan.md Step 0c）：顶点数组是平铺字节流，shader 按 schema 偏移
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

// 顶点属性类型的 packed 字节尺寸（C++/glm 约定：vec3=12B 等）。
// 未知类型返回 0，由校验方报错。
export inline uint32_t vertexAttrSize(const std::string& type) {
	if (type == "float") return 4;
	if (type == "vec2") return 8;
	if (type == "vec3") return 12;
	if (type == "vec4") return 16;
	if (type == "ivec2") return 8;
	if (type == "ivec3") return 12;
	if (type == "ivec4") return 16;
	return 0;
}

// 顶点属性类型 → 生成代码中的 C++ 类型（未知类型返回 nullptr）。
export inline const char* vertexAttrCppType(const std::string& type) {
	if (type == "float") return "float";
	if (type == "vec2") return "glm::vec2";
	if (type == "vec3") return "glm::vec3";
	if (type == "vec4") return "glm::vec4";
	if (type == "ivec2") return "glm::ivec2";
	if (type == "ivec3") return "glm::ivec3";
	if (type == "ivec4") return "glm::ivec4";
	return nullptr;
}

export inline bool has(const nlohmann::json& obj, std::string_view key) {
	return obj.contains(key);
}

export inline const char* getString(const nlohmann::json& obj, std::string_view key) {
	if (!has(obj, key)) return nullptr;
	return obj[key].get_ref<const std::string&>().c_str();
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
