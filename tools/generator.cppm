module;

#include <inja/inja.hpp>
#include <nlohmann/json.hpp>

export module generator;

import rlstd;
import rllogger;
import schema_loader;

namespace RGL {

// GLSL/Slang 标准内存布局（spec 42.6）的 size/alignment 查询表：
// 用于给生成的 C++ 结构体计算字段偏移并插入 padding 字段。
// 必要性：Slang 按标准布局排字段（float4 对齐 16），而 glm 的默认自然对齐
// 对 vec4/mat 只有 4——不插 padding 时 C++ 侧字段偏移/总尺寸与反射不一致
//（如 ModelDrawArgs：uint 后紧跟 float4，C++ 84B vs Slang 96B）。
// 插入 uint32_t padding 后 C++ 偏移被钉死在标准偏移上（padding 尺寸 = 偏移差，
// 且差值恒为 4 的倍数，uint32_t 自身对齐 4 不会再引入额外 padding）。
static bool standardLayoutInfo(const std::string& cppType, size_t& outSize, size_t& outAlign) {
	if (cppType == "float" || cppType == "int32_t" || cppType == "uint32_t") {
		outSize = 4;
		outAlign = 4;
		return true;
	}
	if (cppType == "glm::vec2" || cppType == "glm::ivec2") {
		outSize = 8;
		outAlign = 8;
		return true;
	}
	if (cppType == "glm::vec3" || cppType == "glm::ivec3") {
		outSize = 12;
		outAlign = 4;
		return true;
	}
	if (cppType == "glm::vec4" || cppType == "glm::ivec4") {
		outSize = 16;
		outAlign = 16;
		return true;
	}
	if (cppType == "glm::mat2") {
		outSize = 16;
		outAlign = 16;
		return true;
	}
	if (cppType == "glm::mat3") {
		outSize = 32;
		outAlign = 16;
		return true;
	}
	if (cppType == "glm::mat4") {
		outSize = 64;
		outAlign = 16;
		return true;
	}
	return false;
}

static std::string toCppType(const nlohmann::json& fieldType) {
	if (!has(fieldType, "kind")) return "void";
	const char* kind = getString(fieldType, "kind");
	if (!kind) return "void";

	if (std::strcmp(kind, "matrix") == 0) {
		int rc = has(fieldType, "rowCount") ? fieldType["rowCount"].get<int>() : 0;
		int cc = has(fieldType, "columnCount") ? fieldType["columnCount"].get<int>() : 0;
		if (rc == cc) {
			return "glm::mat" + std::to_string(rc);
		}
		return "glm::mat" + std::to_string(rc) + "x" + std::to_string(cc);
	}
	if (std::strcmp(kind, "vector") == 0) {
		int dim = has(fieldType, "elementCount") ? fieldType["elementCount"].get<int>() : 0;
		const nlohmann::json& elem = fieldType["elementType"];
		const char* st = has(elem, "scalarType") ? getString(elem, "scalarType") : "float32";
		std::string prefix;
		if (std::strcmp(st, "int32") == 0) prefix = "i";
		if (std::strcmp(st, "uint32") == 0) prefix = "u";
		return "glm::" + prefix + "vec" + std::to_string(dim);
	}
	if (std::strcmp(kind, "scalar") == 0) {
		const char* st = has(fieldType, "scalarType") ? getString(fieldType, "scalarType") : "float32";
		if (std::strcmp(st, "float32") == 0) return "float";
		if (std::strcmp(st, "int32") == 0) return "int32_t";
		if (std::strcmp(st, "uint32") == 0) return "uint32_t";
		if (std::strcmp(st, "bool") == 0) return "bool";
	}
	return "void";
}

export std::string generateCppm(const nlohmann::json& reflection,
	const Schema& schema,
	const std::string& templatePath,
	const std::string& moduleName) {
	auto* logger = RLLogger::getInstance();
	logger->debug("Generating .cppm module '{}' from template '{}'", moduleName, templatePath);

	nlohmann::json data;
	data["module_name"] = moduleName;
	data["structs"] = nlohmann::json::array();
	data["slots"] = nlohmann::json::array();

	auto& params = reflection["parameters"];
	std::set<std::string> emittedStructs;

	for (const auto& p : params) {
		if (!has(p, "name") || !has(p, "binding") || !has(p, "type")) continue;
		std::string name = p["name"];
		const nlohmann::json& binding = p["binding"];
		const char* kind = getString(binding, "kind");
		if (!kind) continue;

		if (std::strcmp(kind, "pushConstantBuffer") == 0) {
			const nlohmann::json& type = p["type"];
			const nlohmann::json* elemType = nullptr;
			if (has(type, "elementType")) elemType = &type["elementType"];
			if (!elemType || !has(*elemType, "name")) continue;

			std::string structName = (*elemType)["name"].get<std::string>();
			if (emittedStructs.count(structName)) continue;
			emittedStructs.insert(structName);

			size_t totalSize = 0;
			if (has(type, "elementVarLayout") &&
				has(type["elementVarLayout"], "binding") &&
				has(type["elementVarLayout"]["binding"], "size")) {
				totalSize = type["elementVarLayout"]["binding"]["size"].get<size_t>();
			} else if (has(type, "containerVarLayout") &&
					   has(type["containerVarLayout"], "binding") &&
					   has(type["containerVarLayout"]["binding"], "size")) {
				totalSize = type["containerVarLayout"]["binding"]["size"].get<size_t>();
			}

			nlohmann::json structObj;
			structObj["name"] = structName;
			structObj["fields"] = nlohmann::json::array();
			structObj["size"] = totalSize;

			if (has(*elemType, "fields")) {
				const nlohmann::json& fields = (*elemType)["fields"];
				// 按标准布局计算偏移，必要时插入 uint32_t padding 字段
				size_t cursor = 0;
				size_t structAlign = 4;
				int padCount = 0;
				for (const auto& f : fields) {
					if (!has(f, "name") || !has(f, "type")) continue;
					const std::string cppType = toCppType(f["type"]);
					size_t fieldSize = 0, fieldAlign = 0;
					if (!standardLayoutInfo(cppType, fieldSize, fieldAlign)) {
						logger->error("[FAIL] {} field '{}' has unmappable type '{}'", structName, f["name"].get<std::string>(), cppType);
						continue;
					}
					const size_t offset = (cursor + fieldAlign - 1) / fieldAlign * fieldAlign;
					if (offset > cursor) {
						// padding 用 uint8_t 数组（对齐 1，任意字节数，不会引入额外 padding）
						nlohmann::json padObj;
						padObj["name"] = "_pad" + std::to_string(padCount++) +
							"[" + std::to_string(offset - cursor) + "]";
						padObj["cpp_type"] = "uint8_t";
						structObj["fields"].push_back(padObj);
						cursor = offset;
					}
					nlohmann::json fieldObj;
					fieldObj["name"] = f["name"].get<std::string>();
					fieldObj["cpp_type"] = cppType;
					structObj["fields"].push_back(fieldObj);
					cursor += fieldSize;
					structAlign = std::max(structAlign, fieldAlign);
				}
				// 总尺寸向上取整到结构体对齐（标准布局规则）
				if (totalSize > 0) {
					const size_t padded = (cursor + structAlign - 1) / structAlign * structAlign;
					if (padded > cursor) {
						nlohmann::json padObj;
						padObj["name"] = "_pad" + std::to_string(padCount);
						padObj["cpp_type"] = "uint32_t";
						structObj["fields"].push_back(padObj);
					}
					if (padded != totalSize) {
						logger->error("[FAIL] {} standard layout size {} != reflection size {}", structName, padded, totalSize);
					}
				}
			}

			data["structs"].push_back(structObj);
			logger->debug("Generated struct '{}' with {} fields, size={}", structName, structObj["fields"].size(), totalSize);

		} else if (std::strcmp(kind, "descriptorTableSlot") == 0) {
			for (const auto& ds : schema.descriptorSlots) {
				if (ds.name == name) {
					nlohmann::json slotObj;
					slotObj["cpp_name"] = ds.cppName;
					slotObj["heap_slot_offset"] = ds.heapSlotOffset;
					data["slots"].push_back(slotObj);
					logger->debug("Generated slot '{}' -> {} = {}", name, ds.cppName, ds.heapSlotOffset);
					break;
				}
			}
		}
	}

	// vertexLayouts：反射端无对应数据，直接由 schema 声明生成（契约 source of truth）。
	// 生成的结构体按 packed 布局声明，模板中 static_assert(sizeof == stride) 兜底。
	for (const auto& vl : schema.vertexLayouts) {
		if (vl.name.empty()) continue;
		nlohmann::json structObj;
		structObj["name"] = vl.name;
		structObj["fields"] = nlohmann::json::array();
		structObj["size"] = vl.stride;
		for (const auto& attr : vl.attributes) {
			const char* cppType = vertexAttrCppType(attr.type);
			if (!cppType) continue;
			nlohmann::json fieldObj;
			fieldObj["name"] = attr.name;
			fieldObj["cpp_type"] = cppType;
			structObj["fields"].push_back(fieldObj);
		}
		data["structs"].push_back(structObj);
		logger->debug("Generated vertex struct '{}' with {} fields, stride={}", vl.name, structObj["fields"].size(), vl.stride);
	}

	inja::Environment env;
	std::string result = env.render_file(templatePath, data);
	logger->debug("inja template rendered, {} bytes", result.size());
	return result;
}

export std::string renderTemplate(const nlohmann::json& data,
	const std::string& templatePath) {
	auto* logger = RLLogger::getInstance();
	logger->debug("Rendering template '{}'", templatePath);
	inja::Environment env;
	return env.render_file(templatePath, data);
}

}  // namespace RGL
