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
	if (type.columns > 1) return "mat" + std::to_string(type.vecsize);
	return (type.vecsize > 1 ? "vec" : "") + std::to_string(type.vecsize);
    case spirv_cross::SPIRType::Int:
	return (type.vecsize > 1 ? "ivec" : "int") + std::to_string(type.vecsize);
    case spirv_cross::SPIRType::UInt:
	return (type.vecsize > 1 ? "uvec" : "uint") + std::to_string(type.vecsize);
    case spirv_cross::SPIRType::Struct:
	return compiler.get_name(type.self);
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
    for (const auto& resource : resources.storage_buffers) {
	json ssbo;
	const auto type = compiler.get_type(resource.base_type_id);
	ssbo["name"] = compiler.get_name(resource.base_type_id);
	ssbo["instance_name"] = resource.name;
	ssbo["binding"] = compiler.get_decoration(resource.id, spv::DecorationBinding);
	spirv_cross::Bitset flags = compiler.get_buffer_block_flags(resource.id);
	ssbo["readonly"] = flags.get(spv::DecorationNonWritable);
	const auto& struct_type = compiler.get_type(type.member_types[0]);
	ssbo["struct_name"] = compiler.get_name(struct_type.self);
	ssbo["struct_members"] = json::array();
	for (uint32_t i = 0; i < struct_type.member_types.size(); ++i) {
	    json member;
	    member["name"] = compiler.get_member_name(struct_type.self, i);
	    const auto& member_type = compiler.get_type(struct_type.member_types[i]);
	    member["type"] = type_to_string(compiler, member_type);
	    member["offset"] = compiler.get_member_decoration(struct_type.self, i, spv::DecorationOffset);
	    member["size"] = compiler.get_declared_struct_member_size(struct_type, i);
	    ssbo["struct_members"].push_back(member);
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

