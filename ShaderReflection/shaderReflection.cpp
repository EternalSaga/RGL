#include "shaderReflection.hpp"

#include <algorithm>
#include <cstddef>
#include <spirv.hpp>
#include <spirv_common.hpp>

#include <spirv_glsl.hpp>
#include <string>
#include <vector>
#include <fstream>
#include <set>
#include "rllogger.hpp"

namespace RGL {
namespace reflection {

std::vector<uint32_t> read_spirv_from_file(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
	throw std::runtime_error("Failed to open file: " + filename);
    }

    size_t fileSize = (size_t)file.tellg();
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));

    file.seekg(0);
    file.read((char*)buffer.data(), fileSize);
    file.close();

    return buffer;
}

std::string shaderStage2String(const spv::ExecutionModel& shaderStage) {
    std::string shaderStageStr;
    switch (shaderStage) {
    case spv::ExecutionModelVertex:
	shaderStageStr = "VERTEX";
	break;
    case spv::ExecutionModelFragment:
	shaderStageStr = "FRAGMENT";
	break;
    case spv::ExecutionModelGLCompute:
	shaderStageStr = "COMPUTE";
	break;
    case spv::ExecutionModelGeometry:
	shaderStageStr = "GEOMETRY";
	break;
    case spv::ExecutionModelTessellationControl:
	shaderStageStr = "TESS_CONTROL";
	break;
    case spv::ExecutionModelTessellationEvaluation:
	shaderStageStr = "TESS_EVALUATION";
	break;
    default:
	shaderStageStr = "UNKNOWN_SHADER_STAGE";
	break;
    }
    return shaderStageStr;
}

std::string type_to_string(const spirv_cross::Compiler& compiler, const spirv_cross::SPIRType& type) {
    switch (type.basetype) {
    case spirv_cross::SPIRType::Float:
	if (type.columns > 1) {
	    return "mat" + std::to_string(type.vecsize);
	}
	if (type.vecsize == 1) {
	    return "float";
	}
	return "vec" + std::to_string(type.vecsize);

    case spirv_cross::SPIRType::Int:
	if (type.vecsize == 1) {
	    return "int";
	}
	return "ivec" + std::to_string(type.vecsize);

    case spirv_cross::SPIRType::UInt:
	if (type.vecsize == 1) {
	    return "uint";
	}
	return "uvec" + std::to_string(type.vecsize);

    case spirv_cross::SPIRType::Struct:
	return compiler.get_name(type.self);
    case spirv_cross::SPIRType::SampledImage: {
	std::string sampler_type_str = "sampler";
	switch (type.image.dim) {
	case spv::Dim::Dim1D:
	    sampler_type_str += "1D";
	    break;
	case spv::Dim::Dim2D:
	    sampler_type_str += "2D";
	    break;
	case spv::Dim::Dim3D:
	    sampler_type_str += "3D";
	    break;
	case spv::Dim::DimCube:
	    sampler_type_str += "Cube";
	    break;
	case spv::Dim::DimRect:
	    sampler_type_str += "Rect";
	    break;
	case spv::Dim::DimBuffer:
	    sampler_type_str += "Buffer";
	    break;
	default:
	    sampler_type_str += "Unknown";
	    break;
	}
	if (type.image.arrayed) {
	    sampler_type_str += "Array";
	}
	return sampler_type_str;
    }
    default:
	return "unknown";
    }
}

std::string base_type_to_string(const spirv_cross::SPIRType& type) {
    switch (type.basetype) {
    case spirv_cross::SPIRType::Float:
	return "float";
    case spirv_cross::SPIRType::Int:
	return "int";
    case spirv_cross::SPIRType::UInt:
	return "uint";
    default:
	return "unknown";
    }
}
json ShaderReflection::getInputs() {
    json inputs = json::array();
    for (const auto& resource : resources.stage_inputs) {
	json input;
	input["name"] = resource.name;
	input["location"] = compiler.get_decoration(resource.id, spv::DecorationLocation);
	const auto& type = compiler.get_type(resource.base_type_id);
	input["type"] = type_to_string(compiler, type);
	input["component_type"] = base_type_to_string(type);
	input["component_count"] = type.vecsize;
	inputs.push_back(input);
    }
    return inputs;
}

ShaderReflection::ShaderReflection(std::string spirv_path) : compiler(read_spirv_from_file(spirv_path)) {
    resources = compiler.get_shader_resources();
    j["spirv_path"] = spirv_path;
    j["shader_type"] = shaderStage2String(compiler.get_execution_model());
    j["inputs"] = getInputs();
    j["ubos"] = getUniforms();
    j["storage_buffers"] = getStorageBuffers();
    j["samplers"] = getSamplers(j["ubos"]);

    // 检查文件名里的'-'，replace成'_'
    auto shaderName = std::filesystem::path(spirv_path).stem().string();
    if (shaderName.find('-') != std::string::npos) {
	auto oldName = shaderName;
	std::replace(shaderName.begin(), shaderName.end(), '-', '_');
	RGL::RLLogger::getInstance()->warn("Shader name contains '-', replaced with '_' in shader name \n original name {}, current name {}", oldName, shaderName);
    }
    j["shader_name"] = shaderName;
}

json ShaderReflection::getSamplers(const json& processed_uniforms) {
    json samplers = json::array();
    std::set<uint32_t> processed_bindings;  // 防止重复 Binding

    for (const auto& resource : resources.sampled_images) {
	// 1. 获取 Binding 并去重 (对应 Python set 逻辑)
	uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
	if (processed_bindings.count(binding)) {
	    continue;
	}
	processed_bindings.insert(binding);

	json sampler;
	sampler["name"] = resource.name;
	sampler["binding"] = binding;
	sampler["set"] = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);

	const auto& type = compiler.get_type(resource.type_id);

	// 获取类型字符串
	std::string type_str = type_to_string(compiler, type);
	sampler["type"] = type_str;

	// 判断是否为数组
	bool is_array = !type.array.empty();
	sampler["isArray"] = is_array;

	// 如果是数组，根据 SPIR-V 获取声明的长度（默认情况）
	// 如果是 Runtime Array (Unsized)，这里通常是 0
	if (is_array) {
	    sampler["length"] = type.array[0] ? type.array[0] : 0;
	}

	// ==========================================
	// 融合 Python 的核心逻辑
	// ==========================================
	// Python: if item["type"] == "sampler2D" and item["isArray"]:
	if (type_str == "sampler2D" && is_array) {
	    // Python: for ubo in data["uniforms"]: if ubo["name"] == "MaterialIndices":
	    // 我们遍历传入的 processed_uniforms 寻找目标
	    for (const auto& ubo : processed_uniforms) {
		if (ubo.value("name", "") == "MaterialIndices") {
		    // Python: item["length"] = ubo["members_count"]
		    // 这里的 members_count 包含了我们在上一步 C++ getUniforms 里计算的 padding
		    if (ubo.contains("members_count")) {
			sampler["length"] = ubo["members_count"];
		    }
		    break;  // 找到了就退出内层循环
		}
	    }
	}
	// ==========================================

	samplers.push_back(sampler);
    }
    return samplers;
}

json ShaderReflection::getStorageBuffers() {
    json ssbos = json::array();
    spirv_cross::ShaderResources resources = compiler.get_shader_resources();

    for (const auto& resource : resources.storage_buffers) {
	json ssbo;
	const auto& block_type = compiler.get_type(resource.base_type_id);

	ssbo["block_name"] = compiler.get_name(resource.base_type_id);
	ssbo["instance_name"] = compiler.get_name(resource.id);
	ssbo["binding"] = compiler.get_decoration(resource.id, spv::DecorationBinding);

	spirv_cross::Bitset flags = compiler.get_buffer_block_flags(resource.id);
	ssbo["readonly"] = flags.get(spv::DecorationNonWritable);

	ssbo["block_size_bytes"] = compiler.get_declared_struct_size(block_type);

	auto zeroSize = compiler.get_declared_struct_size_runtime_array(block_type, 0);
	auto oneSize = compiler.get_declared_struct_size_runtime_array(block_type, 1);

	bool hasRuntimeArray = zeroSize != oneSize;

	auto memberCount = block_type.member_types.size();

	ssbo["struct_members"] = json::array();

	// 遍历块（顶级结构体）的所有成员
	for (uint32_t i = 0; i < memberCount; ++i) {
	    json member_info;
	    auto member_type_id = block_type.member_types[i];
	    const spirv_cross::SPIRType& member_type = compiler.get_type(member_type_id);

	    member_info["name"] = compiler.get_member_name(block_type.self, i);
	    member_info["offset"] = compiler.get_member_decoration(block_type.self, i, spv::DecorationOffset);

	    member_info["is_array"] = !member_type.array.empty();
	    if (member_info["is_array"]) {
		bool is_runtime_array = (i == memberCount - 1) && hasRuntimeArray;
		member_info["is_runtime_array"] = is_runtime_array;

		if (!is_runtime_array) {
		    member_info["array_stride"] = compiler.get_decoration(member_type_id, spv::DecorationArrayStride);
		    // 计算固定数组的元素个数
		    if (!member_type.array.empty()) {
			member_info["array_element_count"] = member_type.array[0];
		    }
		} else {
		    member_info["array_stride"] = oneSize - zeroSize;
		}
	    }
	    member_info["type"] = type_to_string(compiler, member_type);
	    member_info["size_bytes"] = compiler.get_declared_struct_member_size(block_type, i);
	    ssbo["struct_members"].push_back(member_info);
	}
	ssbos.push_back(ssbo);
    }
    return ssbos;
}

json ShaderReflection::getUniforms() {
    json uniforms = json::array();
    std::set<uint32_t> processed_bindings;  // 用于 binding 去重

    for (const auto& resource : resources.uniform_buffers) {
	uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);

	// 1. 对应 Python: if ubo["binding"] not in ubo_bindings
	if (processed_bindings.count(binding)) {
	    continue;
	}
	processed_bindings.insert(binding);

	json uniform;
	const auto& type = compiler.get_type(resource.base_type_id);

	// 基础信息
	uniform["name"] = resource.name;
	uniform["instance_name"] = resource.name;  // 注意：如果有 instance name 这里的逻辑可能需要根据 SPIRV-Cross 调整
	uniform["binding"] = binding;

	size_t total_ubo_size = compiler.get_declared_struct_size(type);
	uniform["size_bytes"] = total_ubo_size;

	// 2. 收集原始成员信息到临时 vector 中，方便排序
	struct MemberInfo {
	    json data;
	    size_t offset;
	    size_t size;
	};
	std::vector<MemberInfo> raw_members;

	for (size_t i = 0; i < type.member_types.size(); i++) {
	    json member;
	    member["name"] = compiler.get_member_name(type.self, i);

	    const auto& member_type = compiler.get_type(type.member_types[i]);
	    member["type"] = type_to_string(compiler, member_type);  // 假设你已有这个辅助函数
	    member["is_padding"] = false;			     // 标记这是真实成员

	    size_t offset = compiler.get_member_decoration(type.self, i, spv::DecorationOffset);
	    size_t size = compiler.get_declared_struct_member_size(type, i);

	    member["offset_bytes"] = offset;
	    member["size_bytes"] = size;

	    raw_members.push_back({member, offset, size});
	}

	// 3. 对应 Python: sorted(..., key=lambda m: m["offset_bytes"])
	// 虽然 SPIR-V 通常按 index 顺序就是 offset 顺序，但显式排序更安全
	std::sort(raw_members.begin(), raw_members.end(), [](const MemberInfo& a, const MemberInfo& b) {
	    return a.offset < b.offset;
	});

	// 4. 生成带 Padding 的最终列表
	json final_members = json::array();
	size_t current_offset = 0;
	int padding_index = 0;

	auto add_padding = [&](size_t needed_size) {
	    json pad;
	    pad["is_padding"] = true;
	    pad["name"] = "padding_" + std::to_string(padding_index++);
	    pad["size_bytes"] = needed_size;
	    // padding 一般不需要 type，或者可以在 inja 里判断 is_padding
	    pad["type"] = "char";
	    pad["offset_bytes"] = current_offset;  // 可选：记录 padding 的起始位置
	    final_members.push_back(pad);
	};

	for (const auto& item : raw_members) {
	    // 检查成员前的 Padding
	    if (item.offset > current_offset) {
		add_padding(item.offset - current_offset);
	    }

	    // 添加原始成员
	    final_members.push_back(item.data);

	    // 更新 offset
	    current_offset = item.offset + item.size;
	}

	// 5. 检查结构体末尾的 Padding
	if (total_ubo_size > current_offset) {
	    add_padding(total_ubo_size - current_offset);
	}

	uniform["members"] = final_members;
	uniform["members_count"] = final_members.size();  // Python 代码里也有这个计数

	uniforms.push_back(uniform);
    }
    return uniforms;
}

}  // namespace reflection
}  // namespace RGL
