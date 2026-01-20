#pragma once
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <stdexcept>

#include <spirv_cross.hpp>
#include <sol/sol.hpp>
namespace RGL {
namespace reflection {
class SamplerException : public std::runtime_error {
   public:
    explicit SamplerException(const std::string& msg);
};

class VertexException : public std::runtime_error {
   public:
    explicit VertexException(const std::string& msg);
};

class CheckSampler{
    const spirv_cross::ShaderResources& resources;
    const spirv_cross::Compiler& compiler;
    std::shared_ptr<sol::state> luaState;
    sol::protected_function_result samplerRule;
    public:
    CheckSampler(const std::filesystem::path samplerRulePath,const spirv_cross::ShaderResources& resources,const spirv_cross::Compiler& compiler);

    void checkMaterialLayout() const;
};

class CheckVertex{
    const spirv_cross::ShaderResources& resources;
    const spirv_cross::Compiler& compiler;
    std::shared_ptr<sol::state> luaState;
    sol::protected_function_result vertexRule;
    public:
    CheckVertex(const std::filesystem::path vertexRulePath,const spirv_cross::ShaderResources& resources,const spirv_cross::Compiler& compiler);

    void check() const;
};

}  // namespace reflection
}  // namespace RGL