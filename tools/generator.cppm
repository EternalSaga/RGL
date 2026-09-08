module;

#include <inja/inja.hpp>
#include <nlohmann/json.hpp>
#include <fmt/format.h>

export module generator;

import rlstd;
import rllogger;
import schema_loader;

namespace RGL {

// Scalar block layout 的 size/alignment 查询表（C/C++ 自然布局规则）：
// 用于给生成的 C++ 结构体计算字段偏移并校验总尺寸。
// 本项目所有 shader 经 -fvk-use-scalar-layout 编译（CMake 统一策略，见
// rgl/CMakeLists.txt 头注释），push constant 用 scalar 布局——
// 与 C 自然布局一致（所有类型对齐 4），故生成的 C++ struct（glm 自然对齐 4）
// 与 shader 逐字段偏移天然吻合，无需 padding 字段（DrawArgs = 96B，无 _padN）。
// standardLayoutInfo 的对齐恒为 4（scalar 对齐）；offset 计算/尾 padding 逻辑保留
// 作结构校验（scalar 下不会触发，若触发 = 布局假设被破坏，static_assert 兜底）。
// 注意：若某日取消 scalar 策略改回 GLSL 标准布局（vec4 对齐 16 / vec2 对齐 8），
// 本表与 padding 逻辑须同步改回（历史版本见 git：standardLayoutInfo 原为标准布局表）。
static bool standardLayoutInfo(const std::string& cppType, size_t& outSize, size_t& outAlign) {
	// scalar 布局：所有类型对齐 4（C 自然对齐）
	outAlign = 4;
	if (cppType == "float" || cppType == "int32_t" || cppType == "uint32_t") {
		outSize = 4;
		return true;
	}
	if (cppType == "glm::vec2" || cppType == "glm::ivec2") {
		outSize = 8;
		return true;
	}
	if (cppType == "glm::vec3" || cppType == "glm::ivec3") {
		outSize = 12;
		return true;
	}
	if (cppType == "glm::vec4" || cppType == "glm::ivec4") {
		outSize = 16;
		return true;
	}
	if (cppType == "glm::mat2") {
		outSize = 16;
		return true;
	}
	if (cppType == "glm::mat3") {
		outSize = 32;
		return true;
	}
	if (cppType == "glm::mat4") {
		outSize = 64;
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
			return fmt::format("glm::mat{}", rc);
		}
		return fmt::format("glm::mat{}x{}", rc, cc);
	}
	if (std::strcmp(kind, "vector") == 0) {
		int dim = has(fieldType, "elementCount") ? fieldType["elementCount"].get<int>() : 0;
		const nlohmann::json& elem = fieldType["elementType"];
		const char* st = has(elem, "scalarType") ? getString(elem, "scalarType") : "float32";
		std::string prefix;
		if (std::strcmp(st, "int32") == 0) prefix = "i";
		if (std::strcmp(st, "uint32") == 0) prefix = "u";
		return fmt::format("glm::{}vec{}", prefix, dim);
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

// 返回 nullopt = 致命校验失败（布局漂移/类型不可映射），调用方必须中止（exit 1）。
// 逐字段校验真值 = 反射 JSON 的 binding.offset/binding.size（slangc 按其实际所用
// flag 计算的偏移——即"shader 端真实布局"）：与 standardLayoutInfo 计算值交叉比对，
// 不等 = scalar flag 策略被破坏（如某处 slangc 调用丢 -fvk-use-scalar-layout）或
// 布局表失同步。旧实现只用总尺寸 static_assert 兜底：SPV 侧单点丢 flag 时总尺寸
// 恰好一致的场景（字段调换 + padding 抵消）零检测，此处逐字段堵死。
export std::optional<std::string> generateCppm(const nlohmann::json& reflection,
	const Schema& schema,
	const std::string& templatePath,
	const std::string& moduleName) {
	auto* logger = RLLogger::getInstance();
	logger->debug("Generating .cppm module '{}' from template '{}'", moduleName, templatePath);

	nlohmann::json data;
	data["module_name"] = moduleName;
	data["structs"] = nlohmann::json::array();
	data["slots"] = nlohmann::json::array();
	data["constants"] = nlohmann::json::array();

	auto& params = reflection["parameters"];
	std::set<std::string> emittedStructs;
	bool fatal = false;

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
				// 逐字段双端校验：反射 binding.offset/size（slangc 按其实际 flag 算的
				// 真值）vs standardLayoutInfo 计算值。真值缺失（老版反射 JSON）时跳过
				// 比对仅生成（向后兼容），存在则必须一致，否则致命（flag/表漂移）。
				size_t cursor = 0;
				size_t structAlign = 4;
				int padCount = 0;
				for (const auto& f : fields) {
					if (!has(f, "name") || !has(f, "type")) continue;
					const std::string cppType = toCppType(f["type"]);
					size_t fieldSize = 0, fieldAlign = 0;
					if (!standardLayoutInfo(cppType, fieldSize, fieldAlign)) {
						logger->error("[FAIL] {} field '{}' has unmappable type '{}'", structName, f["name"].get<std::string>(), cppType);
						fatal = true;
						continue;
					}
					const size_t offset = (cursor + fieldAlign - 1) / fieldAlign * fieldAlign;
					if (offset > cursor) {
						// padding 用 uint8_t 数组（对齐 1，任意字节数，不会引入额外 padding）
						nlohmann::json padObj;
						padObj["name"] = fmt::format("_pad{}[{}]", padCount++, offset - cursor);
						padObj["cpp_type"] = "uint8_t";
						padObj["offset"] = nullptr;  // 模板真值判断用（无反射真值）
						structObj["fields"].push_back(padObj);
						cursor = offset;
					}
					nlohmann::json fieldObj;
					fieldObj["name"] = f["name"].get<std::string>();
					fieldObj["cpp_type"] = cppType;
					fieldObj["offset"] = nullptr;  // 恒输出（模板真值判断；老版反射 JSON 无 binding.offset）
					// 反射真值存在则逐字段交叉校验 + 钉进生成物（offsetof static_assert）
					if (has(f, "binding") && has(f["binding"], "offset")) {
						const size_t reflOffset = f["binding"]["offset"].get<size_t>();
						if (reflOffset != offset) {
							logger->error("[FAIL] {} field '{}': computed offset {} != reflection offset {} "
								"(scalar layout flag mismatch between reflection and layout table?)",
								structName, f["name"].get<std::string>(), offset, reflOffset);
							fatal = true;
						}
						if (has(f["binding"], "size") &&
							f["binding"]["size"].get<size_t>() != fieldSize) {
							logger->error("[FAIL] {} field '{}': computed size {} != reflection size {}",
								structName, f["name"].get<std::string>(), fieldSize,
								f["binding"]["size"].get<size_t>());
							fatal = true;
						}
						fieldObj["offset"] = reflOffset;
					}
					structObj["fields"].push_back(fieldObj);
					cursor += fieldSize;
					structAlign = std::max(structAlign, fieldAlign);
				}
				// 总尺寸向上取整到结构体对齐（标准布局规则）。尾 padding 用 uint8_t 数组
				// 填精确字节差（对齐 1，不引入额外 padding）——glm 的自然对齐是 4，
				// 单个 uint32_t 只填 4B：C++ sizeof 按自然对齐 4 上取整，差 >4B 时
				// 与反射总尺寸不符（如 DrawArgs 112B：cursor 104 需 8B 尾 padding）。
				if (totalSize > 0) {
					const size_t padded = (cursor + structAlign - 1) / structAlign * structAlign;
					if (padded > cursor) {
						nlohmann::json padObj;
						padObj["name"] = fmt::format("_pad{}[{}]", padCount++, padded - cursor);
						padObj["cpp_type"] = "uint8_t";
						padObj["offset"] = nullptr;  // 模板真值判断用（无反射真值）
						structObj["fields"].push_back(padObj);
					}
					if (padded != totalSize) {
						logger->error("[FAIL] {} standard layout size {} != reflection size {}", structName, padded, totalSize);
						fatal = true;
					}
				}
			}

			data["structs"].push_back(structObj);
			logger->debug("Generated struct '{}' with {} fields, size={}", structName, structObj["fields"].size(), totalSize);

		} else if (std::strcmp(kind, "descriptorTableSlot") == 0) {
			for (const auto& ds : schema.descriptorSlots) {
				if (ds.name == name) {
					// 运行时槽（无 heapSlotOffset，如 Phase 3 纹理/采样器）不生成 C++ 常量
					if (ds.heapSlotOffset >= 0) {
						nlohmann::json slotObj;
						slotObj["cpp_name"] = ds.cppName;
						slotObj["heap_slot_offset"] = ds.heapSlotOffset;
						data["slots"].push_back(slotObj);
						logger->debug("Generated slot '{}' -> {} = {}", name, ds.cppName, ds.heapSlotOffset);
					} else {
						logger->debug("Slot '{}' is runtime-allocated (no constant generated)", name);
					}
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
				fieldObj["offset"] = nullptr;  // 顶点布局无反射端真值（schema 声明），仅总尺寸 static_assert
				structObj["fields"].push_back(fieldObj);
			}
		data["structs"].push_back(structObj);
		logger->debug("Generated vertex struct '{}' with {} fields, stride={}", vl.name, structObj["fields"].size(), vl.stride);
	}

	// constants：反射端无对应数据，直接由 schema 声明生成（契约 source of truth）。
	for (const auto& ct : schema.constants) {
		if (ct.name.empty()) continue;
		nlohmann::json constObj;
		constObj["cpp_name"] = ct.name;
		constObj["value"] = ct.value;
		data["constants"].push_back(constObj);
		logger->debug("Generated constant '{}' = {}", ct.name, ct.value);
	}

	if (fatal) {
		logger->error("Generation aborted: layout validation failed (see [FAIL] entries above)");
		return std::nullopt;
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
