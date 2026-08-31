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
						const char* kind = has(t, "kind") ? getString(t, "kind") : nullptr;
						if (ef.type == "matrix") {
							if (!kind || std::strcmp(kind, "matrix") != 0) {
								logger->error("[FAIL] {}.{} expected matrix, got kind '{}'", pc.name, ef.name,
									kind ? kind : "(none)");
								ok = false;
							} else if (ef.rowCount > 0 && ef.columnCount > 0) {
								int rc = has(t, "rowCount") ? t["rowCount"].get<int>() : 0;
								int cc = has(t, "columnCount") ? t["columnCount"].get<int>() : 0;
								if (rc != ef.rowCount || cc != ef.columnCount) {
									logger->error("[FAIL] {}.{} matrix dimensions mismatch: expected {}x{}, got {}x{}",
										pc.name, ef.name, ef.rowCount, ef.columnCount, rc, cc);
									ok = false;
								}
							}
						} else if (ef.type.rfind("vec", 0) == 0) {
							// vec2/vec3/vec4：kind=vector + elementCount 匹配（float32 元素）
							const int expectedDim = std::stoi(ef.type.substr(3));
							if (!kind || std::strcmp(kind, "vector") != 0) {
								logger->error("[FAIL] {}.{} expected {}, got kind '{}'", pc.name, ef.name,
									ef.type, kind ? kind : "(none)");
								ok = false;
							} else {
								int dim = has(t, "elementCount") ? t["elementCount"].get<int>() : 0;
								if (dim != expectedDim) {
									logger->error("[FAIL] {}.{} vector dimension mismatch: expected {}, got {}",
										pc.name, ef.name, expectedDim, dim);
									ok = false;
								}
							}
						} else {
							// uint/float/int：kind=scalar + scalarType 匹配
							const char* expectedScalar = ef.type == "uint" ? "uint32"
								: (ef.type == "int" ? "int32" : "float32");
							if (!kind || std::strcmp(kind, "scalar") != 0) {
								logger->error("[FAIL] {}.{} expected scalar '{}', got kind '{}'", pc.name, ef.name,
									ef.type, kind ? kind : "(none)");
								ok = false;
							} else if (has(t, "scalarType")) {
								const char* actual = getString(t, "scalarType");
								if (actual && std::strcmp(actual, expectedScalar) != 0) {
									logger->error("[FAIL] {}.{} scalar type mismatch: expected {}, got {}",
										pc.name, ef.name, expectedScalar, actual);
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

		// 反向检查（旧实现只查 schema ⊆ reflection，shader 加字段而 schema 不更时
		// validate PASS → 生成物带新字段、C++ 侧不写、shader 静默读 0——schema 沦为影子）
		std::vector<std::string> schemaNames;
		for (const auto& ef : pc.fields)
			schemaNames.push_back(ef.name);
		for (const auto& af : fields) {
			if (!has(af, "name")) continue;
			const std::string afName = af["name"].get<std::string>();
			if (std::find(schemaNames.begin(), schemaNames.end(), afName) == schemaNames.end()) {
				logger->error("[FAIL] pushConstantBuffer '{}' has field '{}' not declared in schema "
					"(update schema or the generated C++ struct will not match)", pc.name, afName);
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
		// shape 校验：baseShape 优先（byteAddressBuffer/texture2D）；无 baseShape 时回落 type.kind
		//（SamplerState 反射为 {"kind":"samplerState"}，无 baseShape 字段）
		const char* actualShape = has(type, "baseShape") ? getString(type, "baseShape")
			: (has(type, "kind") ? getString(type, "kind") : nullptr);
		if (actualShape && !ds.shape.empty() &&
			std::strcmp(actualShape, ds.shape.c_str()) != 0) {
			logger->error("[FAIL] descriptorTableSlot '{}' shape mismatch: expected '{}', got '{}'",
				ds.name, ds.shape, actualShape);
			ok = false;
		}
		// heapSlotOffset/cppName 成对可选（Phase 3 纹理槽：运行时 allocSlot 分配，无 C++ 常量）：
		// 有 offset → 必须有 cppName（生成常量）；无 offset → 运行时槽，cppName 必须缺省
		if (ds.heapSlotOffset >= 0 && ds.cppName.empty()) {
			logger->error("[FAIL] descriptorSlot '{}' has heapSlotOffset {} but no cppName", ds.name, ds.heapSlotOffset);
			ok = false;
		}
		if (ds.heapSlotOffset < 0 && !ds.cppName.empty()) {
			logger->error("[FAIL] descriptorSlot '{}' has cppName '{}' but no heapSlotOffset（运行时槽不生成常量）", ds.name, ds.cppName);
			ok = false;
		}
	}

	// 反向：shader 声明了 binding 但 schema 未登记 → FAIL（新增 binding 不改 schema
	// 时 C++ 侧 mapping 数组（shader.cppm 定长）不覆盖 → 运行期 createShadersEXT 失败）
	{
		std::vector<std::string> dsNames;
		for (const auto& ds : schema.descriptorSlots)
			dsNames.push_back(ds.name);
		for (const auto& [name, p] : descSlotMap) {
			(void)p;
			if (std::find(dsNames.begin(), dsNames.end(), name) == dsNames.end()) {
				logger->error("[FAIL] descriptorTableSlot '{}' in reflection not declared in schema", name);
				ok = false;
			}
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

	// constants 无反射端对应，只校验内部一致性：name 非空 + value 为正。
	for (const auto& ct : schema.constants) {
		if (ct.name.empty()) {
			logger->error("[FAIL] constant missing name");
			ok = false;
		}
		if (ct.value == 0) {
			logger->error("[FAIL] constant '{}' has zero value", ct.name);
			ok = false;
		}
	}

	return ok;
}

}  // namespace RGL
