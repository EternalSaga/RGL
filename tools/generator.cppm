module;

#include <inja/inja.hpp>
#include <nlohmann/json.hpp>

export module generator;

import rlstd;
import rllogger;
import schema_loader;

namespace RGL {

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
				for (const auto& f : fields) {
					if (!has(f, "name") || !has(f, "type")) continue;
					nlohmann::json fieldObj;
					fieldObj["name"] = f["name"].get<std::string>();
					fieldObj["cpp_type"] = toCppType(f["type"]);
					structObj["fields"].push_back(fieldObj);
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
