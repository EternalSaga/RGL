#include "assimpMaterialBinding.hpp"
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <sol/optional_implementation.hpp>
#include <sol/table.hpp>
#include <string>
#include "luaSandbox.hpp"
#include <fmt/format.h>
namespace RGL::reflection {
AssimpMaterialBinding::AssimpMaterialBinding(const std::filesystem::path& bindOutputPath, const std::filesystem::path& samplerRulePath)
    : bindOutputPath(bindOutputPath) {
    auto luaState = LuaSandbox::getLuaState();
    const auto samplerRule = luaState->script_file(samplerRulePath.generic_string());
    auto samplerRuleJson = lua2json(samplerRule);
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
    }

    return result;
}
}  // namespace RGL::reflection