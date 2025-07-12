#include "shaderReflection.hpp"

#include <cstddef>
#include <spirv.hpp>
#include <spirv_common.hpp>

#include <spirv_glsl.hpp>
#include <vector>
#include <fstream>

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

ShaderReflection::ShaderReflection(std::string spirv_path) : compiler(read_spirv_from_file(spirv_path)) {
    resources = compiler.get_shader_resources();
    j["spirv_path"] = spirv_path;

    switch (compiler.get_execution_model()) {
    case spv::ExecutionModelVertex:
	j["shader_stage"] = "vertex";
	break;
    case spv::ExecutionModelFragment:
	j["shader_stage"] = "fragment";
	break;
    case spv::ExecutionModelGLCompute:
	j["shader_stage"] = "compute";
	break;
    case spv::ExecutionModelGeometry:
	j["shader_stage"] = "geometry";
	break;
    case spv::ExecutionModelTessellationControl:
	j["shader_stage"] = "tessellation_control";
	break;
    case spv::ExecutionModelTessellationEvaluation:
	j["shader_stage"] = "tessellation_evaluation";
	break;
    default:
	j["shader_stage"] = "unknown";
	break;
    }
    j["inputs"] = getInputs();
    j["uniforms"] = getUniforms();
    j["storage_buffers"] = getStorageBuffers();
    j["samplers"] = getSamplers();
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

json ShaderReflection::getSamplers() {
    json samplers = json::array();
    for (const auto& resource : resources.sampled_images) {
	json sampler;
	sampler["name"] = resource.name;
	sampler["binding"] = compiler.get_decoration(resource.id, spv::DecorationBinding);
	const auto& type = compiler.get_type(resource.type_id);
	sampler["type"] = type_to_string(compiler, type);  // e.g., sampler2D
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

		bool hasRuntimeArray = zeroSize!= oneSize;

		auto memberCount = block_type.member_types.size();

        ssbo["struct_members"] = json::array();

        // 遍历块（顶级结构体）的所有成员
        for (uint32_t i = 0; i < memberCount; ++i) {
            json member_info;
            auto member_type_id = block_type.member_types[i];
            const spirv_cross::SPIRType& member_type = compiler.get_type(member_type_id);

            member_info["name"] = compiler.get_member_name(block_type.self, i);
            member_info["offset"] = compiler.get_member_decoration(block_type.self, i, spv::DecorationOffset);

            // --- 核心修正：正确处理类型和数组 ---

            // 1. 判断是否为数组
            member_info["is_array"] = !member_type.array.empty();
            if (member_info["is_array"]) {
                // 2. 如果是数组，判断是否为运行时数组，判断条件：i是最后一个索引且block包含运行时数组
				bool is_runtime_array = (i ==memberCount-1) && hasRuntimeArray;
				member_info["is_runtime_array"]= is_runtime_array;


                // 3. 获取数组元素的大小（步幅）
				if (!is_runtime_array) {
					member_info["array_stride"] = compiler.get_decoration(member_type_id, spv::DecorationArrayStride);
				}else {
					member_info["array_stride"] = oneSize - zeroSize;
				}
            }
            
            // 4. 获取基础类型（无论是数组元素还是普通成员）
            // type_to_string 对于数组类型，会返回其元素类型，这是我们期望的
            member_info["type"] = type_to_string(compiler, member_type);
            
            // 5. 获取成员声明的大小
            // 对于运行时数组，此API返回0。对于固定大小的数组成员，它返回整个数组的大小。
            // 对于非数组成员，它返回成员的大小。
            member_info["size_bytes"] = compiler.get_declared_struct_member_size(block_type, i);
            
            ssbo["struct_members"].push_back(member_info);
        }
        ssbos.push_back(ssbo);
    }
    return ssbos;
}



json ShaderReflection::getUniforms() {
    json uniforms = json::array();
    for (const auto& resource : resources.uniform_buffers) {
	json uniform;
	const auto& type = compiler.get_type(resource.base_type_id);
	uniform["name"] = resource.name;
	uniform["instance_name"] = resource.name;
	uniform["binding"] = compiler.get_decoration(resource.id, spv::DecorationBinding);
	uniform["size_bytes"] = compiler.get_declared_struct_size(type);
	json members = json::array();
	for (size_t i = 0; i < type.member_types.size(); i++) {
	    json member;
	    member["name"] = compiler.get_member_name(type.self, i);
	    const auto& member_type = compiler.get_type(type.member_types[i]);
	    member["type"] = type_to_string(compiler, member_type);
	    member["offset_bytes"] = compiler.get_member_decoration(type.self, i, spv::DecorationOffset);
	    member["size_bytes"] = compiler.get_declared_struct_member_size(type, i);
	    members.push_back(member);
	}
	uniform["members"] = members;
	uniforms.push_back(uniform);
    }
    return uniforms;
}

}  // namespace reflection
}  // namespace RGL
