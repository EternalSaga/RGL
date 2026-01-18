#include "samplerException.hpp"
#include <sol/protected_function_result.hpp>
#include <string>
#include <fmt/format.h>
#include <boost/unordered/unordered_flat_set.hpp>
#include "luaSandbox.hpp"
namespace RGL {
namespace reflection {

SamplerException::SamplerException(const std::string& msg) : std::runtime_error(msg) {}
using json = nlohmann::json;
CheckSampler::CheckSampler(const std::filesystem::path samplerRulePath, const spirv_cross::ShaderResources& resources, const spirv_cross::Compiler& compiler) : resources(resources), compiler(compiler) {
    luaState = LuaSandbox::getLuaState();
    samplerRule = luaState->script_file(samplerRulePath.generic_string());
}

void CheckSampler::checkMaterialLayout() const {

    // 0. 安全性检查：确保 Lua 脚本执行成功并返回了一个 Table
    if (!samplerRule.valid()) {
        sol::error err = samplerRule;
        throw SamplerException(fmt::format("Lua config execution failed: {}", err.what()));
    }

    // 将结果转换为 Table
    sol::table rootConfig = samplerRule;

    // 1. 获取 UBO 名字
    // 注意：Lua 里 key 是 "UniformBlock"
    sol::optional<std::string> uboNameOpt = rootConfig["UniformBlock"];
    if (!uboNameOpt) {
        throw SamplerException("Lua config missing required key: 'UniformBlock'");
    }
    const std::string targetUboName = uboNameOpt.value();

    // 2. 查找 UBO 是否存在 (和之前一样，无需修改)
    const spirv_cross::Resource* targetResource = nullptr;
    for (const auto& resource : resources.uniform_buffers) {
        if (resource.name == targetUboName) {
            targetResource = &resource;
            break;
        }
    }

    if (!targetResource) {
        throw SamplerException(fmt::format("Uniform Block '{}' required by schema is missing in shader.", targetUboName));
    }

    // 3. 构建允许的变量名白名单 (从 Lua 读取)
    boost::unordered::unordered_flat_set<std::string> allowedMemberNames;

    // 路径: Templates -> PBR_Standard -> Parameters
    // 使用 sol::optional 防止路径不存在导致崩溃
    sol::optional<sol::table> templates = rootConfig["Templates"];

    sol::optional<sol::table> pbrStd = sol::nullopt;
    if (templates) {
        pbrStd = templates.value().get<sol::optional<sol::table>>("PBR_Standard");
    }

    sol::optional<sol::table> parameters = sol::nullopt;
    if (pbrStd) {
        parameters = pbrStd.value().get<sol::optional<sol::table>>("Parameters");
    }

    if (parameters) {
        // 遍历 Parameters 数组
        // 在 sol2 中，遍历 array-like table 会得到 (key: int, value: object)
        for (const auto& pair : parameters.value()) {
            // pair.second 是数组里的元素 (即 Sampler 函数返回的那个 table)
            if (pair.second.is<sol::table>()) {
                sol::table samplerConfig = pair.second.as<sol::table>();
                
                // 获取 "shader_variable"
                sol::optional<std::string> varName = samplerConfig["shader_variable"];
                if (varName) {
                    allowedMemberNames.insert(varName.value());
                }
            }
        }
    } else {
         // 可选：如果没找到配置路径，是否要抛异常？
         throw SamplerException("Invalid schema structure: Templates.PBR_Standard.Parameters missing");
    }

    // 4. 【核心修正】检查 UBO 内部的成员变量 (和之前一样)
    const spirv_cross::SPIRType& type = compiler.get_type(targetResource->type_id);
    size_t memberCount = type.member_types.size();

    for (uint32_t i = 0; i < memberCount; i++) {
        std::string memberName = compiler.get_member_name(targetResource->type_id, i);

        // 【新增】处理 UBO Padding 问题
        // 现代编译器可能会生成以 _pad 开头的填充变量，建议跳过检查
        if (memberName.rfind("_pad", 0) == 0) { 
            continue; 
        }

        if (!allowedMemberNames.contains(memberName)) {
            throw SamplerException(fmt::format(
                "Material Property '{}' inside block '{}' is not defined in the Lua Schema.",
                memberName, targetUboName));
        }
    }
}


}  // namespace reflection
}  // namespace RGL