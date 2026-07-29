#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <spirv_cross.hpp>
#include "samplerException.hpp"
namespace RGL {
namespace reflection {
    using json = nlohmann::json;
class ShaderReflection {
    json j;
    spirv_cross::ShaderResources resources;
    spirv_cross::Compiler compiler;
    json getUniforms();

    json getStorageBuffers();

    json getSamplers();

    json getInputs();

    json getStructs();

    std::unique_ptr<CheckSampler> checkSampler;
    std::unique_ptr<CheckVertex> checkVertex;

   public:
    ShaderReflection(std::string spirv_path, const std::filesystem::path samplerRulePath, const std::filesystem::path vertexRulePath);
    inline explicit operator json() const { return j; }
};
}}