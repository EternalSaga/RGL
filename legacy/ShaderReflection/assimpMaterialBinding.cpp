#include "assimpMaterialBinding.hpp"
#include <inja/environment.hpp>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <sol/optional_implementation.hpp>
#include <sol/table.hpp>
#include <string>
#include "injautils.hpp"
#include "luaSandbox.hpp"

#include <fmt/format.h>
#include <inja/inja.hpp>
namespace RGL::reflection {
AssimpMaterialBinding::AssimpMaterialBinding(const std::filesystem::path& bindOutputPath, const std::filesystem::path& samplerRulePath,const std::filesystem::path& jinjaTemplatePath)
    : bindOutputPath(bindOutputPath) {
    auto luaState = LuaSandbox::getLuaState();
    const auto samplerRule = luaState->script_file(samplerRulePath.generic_string());
    auto samplerRuleJson = lua2json(samplerRule);
    fmt::print("Pretty Result:\n{}\n", samplerRuleJson.dump(4));
    generateMaterialBinding(samplerRuleJson, jinjaTemplatePath);
}
void AssimpMaterialBinding::generateMaterialBinding(const nlohmann::json& materialBinding, const std::filesystem::path& jinjaTempaltePath){
    if (bindOutputPath.has_parent_path()) {
        std::filesystem::create_directories(bindOutputPath.parent_path());
    }
    inja::Environment env;
    env.write(jinjaTempaltePath.generic_string(), materialBinding, bindOutputPath.generic_string());
}
nlohmann::json AssimpMaterialBinding::lua2json(const sol::protected_function_result& samplerRule) {
    sol::table samplerRuleTable = samplerRule;
    nlohmann::json result;

    sol::optional<sol::table> pbrStandardParameters = samplerRuleTable["Templates"]["PBR_Standard"]["Parameters"];
    if (!pbrStandardParameters) {
	throw LuaTableExcept(fmt::format("PBR_Standard Parameters not found in sampler rule"));
    }
    for (const auto& pair  : pbrStandardParameters.value()) {
	nlohmann::json itemj;
    sol::table samplerObj = pair.second; 
	for (const auto& samplerPair : samplerObj) {
	    std::string key = samplerPair.first.as<std::string>();

	    if (samplerPair.second.is<std::string>()) {
		itemj[key] = samplerPair.second.as<std::string>();
	    } else if (samplerPair.second == sol::lua_nil) {
		itemj[key] = nullptr;
	    }
	}
       result["Templates"]["PBR_Standard"]["Parameters"].push_back(itemj);

    }
    sol::optional<sol::table> pbrStandard = samplerRuleTable["Templates"]["PBR_Standard"];
    if (!pbrStandard){
        throw LuaTableExcept(fmt::format("PBR_Standard not found"));
    }
    sol::optional<std::string>  assimpBindingName = samplerRuleTable["Templates"]["PBR_Standard"]["AssimpBindingName"];
    if (!assimpBindingName) {
        throw LuaTableExcept(fmt::format("AssimpBindingName not found"));
    }
    result["Templates"]["PBR_Standard"]["AssimpBindingName"] = assimpBindingName.value();
    result["generation_time"] = getCurrentTimestamp();
    return result;
}
}  // namespace RGL::reflection