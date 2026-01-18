#include "shaderReflection.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <spirv.hpp>
#include <spirv_common.hpp>

#include <spirv_glsl.hpp>
#include <stdexcept>
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

ShaderReflection::ShaderReflection(std::string spirv_path, const std::filesystem::path samplerRulePath) : compiler(read_spirv_from_file(spirv_path)) {
    resources = compiler.get_shader_resources();
    checkSampler = std::make_unique<CheckSampler>(samplerRulePath,resources, compiler);
    checkSampler->checkMaterialLayout();
    j["spirv_path"] = spirv_path;
    j["shader_type"] = shaderStage2String(compiler.get_execution_model());
    j["inputs"] = getInputs();
    j["ubos"] = getUniforms();
    j["storage_buffers"] = getStorageBuffers();
    j["samplers"] = getSamplers();


    auto shaderName = std::filesystem::path(spirv_path).stem().string();
    if (shaderName.find('-') != std::string::npos) {
	RGL::RLLogger::getInstance()->error("Shader name contains '-', replaced with '_' in shader name.");
	throw std::runtime_error("Don't use '-' in shader file name!");
    }
    j["shader_name"] = shaderName;

}

json ShaderReflection::getSamplers() {
    json samplers = json::array();
    std::set<uint32_t> processed_bindings;  // 防止重复 Binding

    for (const auto& resource : resources.sampled_images) {

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

	if (is_array) {
	    uint32_t declaredSize = type.array[0];
            
        sampler["isDynamic"] = (declaredSize == 0); // 标记为动态
        sampler["length"] = declaredSize;           // 如果是0，就是0
	}

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

        // 这是不包含运行时数组变长部分的固定大小
        size_t block_size_bytes = compiler.get_declared_struct_size(block_type);
        ssbo["block_size_bytes"] = block_size_bytes;

        // 检查是否存在运行时数组 (Runtime Array)
        auto zeroSize = compiler.get_declared_struct_size_runtime_array(block_type, 0);
        auto oneSize = compiler.get_declared_struct_size_runtime_array(block_type, 1);
        bool hasRuntimeArray = zeroSize != oneSize;

        auto memberCount = block_type.member_types.size();

        // --- 逻辑变更开始：不再直接写入 ssbo["struct_members"] ---

        std::vector<json> raw_fixed_members;
        json runtime_member_json = nullptr; 

        // 1. 遍历并分离成员
        for (uint32_t i = 0; i < memberCount; ++i) {
            json member_info;
            auto member_type_id = block_type.member_types[i];
            const spirv_cross::SPIRType& member_type = compiler.get_type(member_type_id);

            member_info["name"] = compiler.get_member_name(block_type.self, i);
            uint32_t offset = compiler.get_member_decoration(block_type.self, i, spv::DecorationOffset);
            member_info["offset"] = offset;

            member_info["is_array"] = !member_type.array.empty();
            bool is_runtime_array = false;

            if (member_info["is_array"]) {
                // 只有最后一个成员且 block 本身被标记为变长才是 Runtime Array
                is_runtime_array = (i == memberCount - 1) && hasRuntimeArray;
                member_info["is_runtime_array"] = is_runtime_array;

                if (!is_runtime_array) {
                    member_info["array_stride"] = compiler.get_decoration(member_type_id, spv::DecorationArrayStride);
                    if (!member_type.array.empty()) {
                        member_info["array_element_count"] = member_type.array[0];
                    }
                } else {
                    member_info["array_stride"] = oneSize - zeroSize;
                }
            } else {
                 member_info["is_runtime_array"] = false;
            }

            member_info["type"] = type_to_string(compiler, member_type);
            
            // 获取成员大小 (spirv-cross 会正确返回固定数组的总大小)
            size_t member_size = compiler.get_declared_struct_member_size(block_type, i);
            member_info["size_bytes"] = member_size;

            // 分离逻辑：放入不同的容器
            if (is_runtime_array) {
                runtime_member_json = member_info;
            } else {
                // 标记此时还不是 padding，后续统一处理
                member_info["is_padding"] = false;
                raw_fixed_members.push_back(member_info);
            }
        }

        // 2. 对固定成员按 offset 排序 (对应 Python: sorted(fixed_members_raw))
        std::sort(raw_fixed_members.begin(), raw_fixed_members.end(), 
            [](const json& a, const json& b) {
                return a["offset"] < b["offset"];
            });

        // 3. 计算 Padding 并构建最终的 fixed_members (对应 Python: Step 2)
        json processed_fixed_members = json::array();
        size_t current_offset = 0;
        int padding_index = 0;

        for (const auto& member : raw_fixed_members) {
            size_t member_offset = member["offset"];
            size_t member_size = member["size_bytes"];

            // 检查成员之间是否有空隙
            if (member_offset > current_offset) {
                size_t padding_size = member_offset - current_offset;
                json padding_node;
                padding_node["is_padding"] = true;
                padding_node["name"] = "padding_" + std::to_string(padding_index++);
                padding_node["size_bytes"] = padding_size;
                // 为了兼容某些模板，可能需要 offset 字段，虽然通常 padding 不需要
                padding_node["offset"] = current_offset; 
                
                processed_fixed_members.push_back(padding_node);
            }

            // 添加实际成员
            processed_fixed_members.push_back(member);
            
            // 更新 current_offset
            current_offset = member_offset + member_size;
        }

        // 检查固定部分末尾是否需要 Padding (结构体总大小 vs 当前偏移量)
        if (block_size_bytes > current_offset) {
            size_t padding_size = block_size_bytes - current_offset;
            json padding_node;
            padding_node["is_padding"] = true;
            padding_node["name"] = "padding_" + std::to_string(padding_index++);
            padding_node["size_bytes"] = padding_size;
            padding_node["offset"] = current_offset;
            
            processed_fixed_members.push_back(padding_node);
        }

        // 4. 将处理好的数据放回 item 中 (对应 Python: Step 3)
        ssbo["fixed_members"] = processed_fixed_members;
        
        // 如果没有运行时成员，这里显式设为 null 或者不写，视你的模板需求而定
        if (runtime_member_json.is_null()) {
            ssbo["runtime_member"] = nullptr;
        } else {
            ssbo["runtime_member"] = runtime_member_json;
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
