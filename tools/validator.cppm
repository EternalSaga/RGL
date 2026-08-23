module;

#include <nlohmann/json.hpp>

export module validator;

import rlstd;
import rllogger;
import schema_loader;

namespace RGL {

export bool validateSchema(const nlohmann::json& reflection, const Schema& schema) {
	bool ok = true;
	auto* logger = RLLogger::getInstance();
	auto& params = reflection["parameters"];

	std::map<std::string, const nlohmann::json*> pushConstMap;
	std::map<std::string, const nlohmann::json*> descSlotMap;

	for (const auto& p : params) {
		std::string name = p["name"];
		auto binding = p["binding"];
		const char* kind = getString(binding, "kind");
		if (kind) {
			if (std::strcmp(kind, "pushConstantBuffer") == 0) {
				pushConstMap[name] = &p;
			} else if (std::strcmp(kind, "descriptorTableSlot") == 0) {
				descSlotMap[name] = &p;
			}
		}
	}

	for (const auto& pc : schema.pushConstants) {
		auto it = pushConstMap.find(pc.name);
		if (it == pushConstMap.end()) {
			logger->error("[FAIL] pushConstantBuffer '{}' not found in reflection", pc.name);
			ok = false;
			continue;
		}
		const nlohmann::json* p = it->second;
		const nlohmann::json& type = (*p)["type"];
		const nlohmann::json* elemType = nullptr;
		if (has(type, "elementType")) {
			elemType = &type["elementType"];
		} else {
			logger->error("[FAIL] pushConstantBuffer '{}' missing elementType", pc.name);
			ok = false;
			continue;
		}

		if (!has(*elemType, "fields")) {
			logger->error("[FAIL] pushConstantBuffer '{}' elementType has no fields", pc.name);
			ok = false;
			continue;
		}

		const nlohmann::json& fields = (*elemType)["fields"];

		for (const auto& ef : pc.fields) {
			bool found = false;
			for (const auto& af : fields) {
				if (!has(af, "name")) continue;
				if (af["name"].get<std::string>() == ef.name) {
					found = true;
					if (has(af, "type")) {
						const nlohmann::json& t = af["type"];
						if (ef.type == "matrix" && has(t, "kind") &&
							getString(t, "kind") &&
							std::strcmp(getString(t, "kind"), "matrix") == 0) {
							if (ef.rowCount > 0 && ef.columnCount > 0) {
								int rc = has(t, "rowCount") ? t["rowCount"].get<int>() : 0;
								int cc = has(t, "columnCount") ? t["columnCount"].get<int>() : 0;
								if (rc != ef.rowCount || cc != ef.columnCount) {
									logger->error("[FAIL] {}.{} matrix dimensions mismatch: expected {}x{}, got {}x{}",
										pc.name, ef.name, ef.rowCount, ef.columnCount, rc, cc);
									ok = false;
								}
							}
						}
					}
					break;
				}
			}
			if (!found) {
				logger->error("[FAIL] pushConstantBuffer '{}' missing expected field '{}'", pc.name, ef.name);
				ok = false;
			}
		}
	}

	for (const auto& ds : schema.descriptorSlots) {
		auto it = descSlotMap.find(ds.name);
		if (it == descSlotMap.end()) {
			logger->error("[FAIL] descriptorTableSlot '{}' not found in reflection", ds.name);
			ok = false;
			continue;
		}
		const nlohmann::json* p = it->second;
		auto binding = (*p)["binding"];
		if (has(binding, "index")) {
			int actualIdx = binding["index"].get<int>();
			if (actualIdx != ds.index) {
				logger->error("[FAIL] descriptorTableSlot '{}' index mismatch: expected {}, got {}",
					ds.name, ds.index, actualIdx);
				ok = false;
			}
		}
		const nlohmann::json& type = (*p)["type"];
		if (has(type, "baseShape")) {
			const char* actualShape = getString(type, "baseShape");
			if (actualShape && !ds.shape.empty() &&
				std::strcmp(actualShape, ds.shape.c_str()) != 0) {
				logger->error("[FAIL] descriptorTableSlot '{}' shape mismatch: expected '{}', got '{}'",
					ds.name, ds.shape, actualShape);
				ok = false;
			}
		}
		if (ds.heapSlotOffset < 0) {
			logger->error("[FAIL] descriptorSlot '{}' missing heapSlotOffset", ds.name);
			ok = false;
		}
		if (ds.cppName.empty()) {
			logger->error("[FAIL] descriptorSlot '{}' missing cppName", ds.name);
			ok = false;
		}
	}

	// vertexLayouts 反射端没有对应数据（slangc 不反射顶点 buffer 结构体），
	// 只校验内部一致性：类型已知 + 偏移 packed 连续 + stride = 属性尺寸总和。
	for (const auto& vl : schema.vertexLayouts) {
		if (vl.name.empty()) {
			logger->error("[FAIL] vertexLayout missing name");
			ok = false;
			continue;
		}
		if (vl.stride == 0 || vl.attributes.empty()) {
			logger->error("[FAIL] vertexLayout '{}' missing stride or attributes", vl.name);
			ok = false;
			continue;
		}
		uint32_t cursor = 0;
		for (const auto& attr : vl.attributes) {
			uint32_t size = vertexAttrSize(attr.type);
			if (size == 0) {
				logger->error("[FAIL] vertexLayout '{}' attribute '{}' has unknown type '{}'", vl.name, attr.name, attr.type);
				ok = false;
				break;
			}
			if (attr.offset != cursor) {
				logger->error("[FAIL] vertexLayout '{}' attribute '{}' offset {} != expected {} (packed layout)", vl.name, attr.name, attr.offset, cursor);
				ok = false;
			}
			cursor += size;
		}
		if (vl.stride != cursor) {
			logger->error("[FAIL] vertexLayout '{}' stride {} != attributes total {}", vl.name, vl.stride, cursor);
			ok = false;
		}
	}

	return ok;
}

}  // namespace RGL
