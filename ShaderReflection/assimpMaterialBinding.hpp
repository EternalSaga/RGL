#pragma once
#include <filesystem>
#include <memory>

#include <nlohmann/json_fwd.hpp>
#include <sol/protected_function_result.hpp>
#include <sol/sol.hpp>
namespace RGL {
namespace reflection {
    class AssimpMaterialBinding {
        std::shared_ptr<sol::state> luaState;
        std::filesystem::path bindOutputPath;
        nlohmann::json lua2json(const sol::protected_function_result& samplerRule);
        public:
        AssimpMaterialBinding(const std::filesystem::path& bindOutputPath, const std::filesystem::path& samplerRulePath);
        void genBinding();
    };
}
}