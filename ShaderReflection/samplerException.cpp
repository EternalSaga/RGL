#include "samplerException.hpp"
#include <sol/protected_function_result.hpp>
#include <string>
#include <algorithm>
#include <spirv.hpp>
#include <fmt/format.h>
#include <boost/unordered/unordered_flat_set.hpp>
#include "luaSandbox.hpp"
#include "rllogger.hpp"
namespace RGL {
namespace reflection {

SamplerException::SamplerException(const std::string& msg) : std::runtime_error(msg) {}

VertexException::VertexException(const std::string& msg) : std::runtime_error(msg) {}

using json = nlohmann::json;
CheckSampler::CheckSampler(const std::filesystem::path samplerRulePath, const spirv_cross::ShaderResources& resources, const spirv_cross::Compiler& compiler) : resources(resources), compiler(compiler) {
    luaState = LuaSandbox::getLuaState();
    RLLogger::getInstance()->info("Sampler rule path {}",samplerRulePath.generic_string());
    samplerRule = luaState->script_file(samplerRulePath.generic_string());
}

void CheckSampler::checkMaterialLayout() const {

    if (!samplerRule.valid()) {
        sol::error err = samplerRule;
        throw SamplerException(fmt::format("Lua config execution failed: {}", err.what()));
    }

    sol::table rootConfig = samplerRule;

    sol::optional<std::string> uboNameOpt = rootConfig["UniformBlock"];
    if (!uboNameOpt) {
        throw SamplerException("Lua config missing required key: 'UniformBlock'");
    }
    const std::string targetUboName = uboNameOpt.value();

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

    boost::unordered::unordered_flat_set<std::string> allowedMemberNames;

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

        for (const auto& pair : parameters.value()) {

            if (pair.second.is<sol::table>()) {
                sol::table samplerConfig = pair.second.as<sol::table>();

                sol::optional<std::string> varName = samplerConfig["shader_variable"];
                if (varName) {
                    allowedMemberNames.insert(varName.value());
                }
            }
        }
    } else {

         throw SamplerException("Invalid schema structure: Templates.PBR_Standard.Parameters missing");
    }

    const spirv_cross::SPIRType& type = compiler.get_type(targetResource->base_type_id);
    size_t memberCount = type.member_types.size();

    for (uint32_t i = 0; i < memberCount; i++) {
        std::string memberName = compiler.get_member_name(type.self, i);
        if (!allowedMemberNames.contains(memberName)) {
            throw SamplerException(fmt::format(
                "Material Property '{}' inside block '{}' is not defined in the Lua Schema.",
                memberName, targetUboName));
        }
    }
}

CheckVertex::CheckVertex(const std::filesystem::path vertexRulePath, const spirv_cross::ShaderResources& resources, const spirv_cross::Compiler& compiler)
    : resources(resources), compiler(compiler) {
    luaState = LuaSandbox::getLuaState();
    RLLogger::getInstance()->info("Vertex rule path {}", vertexRulePath.generic_string());
    vertexRule = luaState->script_file(vertexRulePath.generic_string());
}

void CheckVertex::check() const {
    if (!vertexRule.valid()) {
        sol::error err = vertexRule;
        throw VertexException(fmt::format("Lua vertex config execution failed: {}", err.what()));
    }

    sol::table rootConfig = vertexRule;
    sol::optional<sol::table> templates = rootConfig["Templates"];
    if (!templates) {
        throw VertexException("Lua vertex config missing 'Templates'");
    }

    sol::optional<sol::table> vertexPulling = templates.value()["Standard_VertexPulling"];
    if (!vertexPulling) {
        throw VertexException("Lua vertex config missing 'Templates.Standard_VertexPulling'");
    }

    auto checkResource = [&](const sol::table& required, const auto& actualResources, const std::string& typeName) {
        std::string name = required["name"];
        uint32_t expectedBinding = required["binding"];
        bool optional = required.get_or("optional", false);

        auto it = std::find_if(actualResources.begin(), actualResources.end(), [&](const spirv_cross::Resource& res) {
            return res.name == name;
        });

        if (it == actualResources.end()) {
            if (!optional) {
                throw VertexException(fmt::format("Required {} '{}' is missing in shader.", typeName, name));
            }
            return;
        }

        uint32_t actualBinding = compiler.get_decoration(it->id, spv::DecorationBinding);
        if (actualBinding != expectedBinding) {
            throw VertexException(fmt::format("{} '{}' has binding {}, but schema requires {}.", typeName, name, actualBinding, expectedBinding));
        }
    };

    sol::optional<sol::table> storageBuffers = (*vertexPulling)["StorageBuffers"];
    if (storageBuffers) {
        for (const auto& pair : *storageBuffers) {
            if (pair.second.is<sol::table>()) {
                checkResource(pair.second.as<sol::table>(), resources.storage_buffers, "Storage Buffer");
            }
        }
    }

    sol::optional<sol::table> uniforms = (*vertexPulling)["Uniforms"];
    if (uniforms) {
        for (const auto& pair : *uniforms) {
            if (pair.second.is<sol::table>()) {
                checkResource(pair.second.as<sol::table>(), resources.uniform_buffers, "Uniform Block");
            }
        }
    }
}


}  // namespace reflection
}  // namespace RGL