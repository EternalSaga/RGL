#pragma once
#include <nlohmann/json.hpp>
#include <spirv_cross.hpp>
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

   public:
    ShaderReflection(std::string spirv_path);
    inline explicit operator json() const { return j; }
};
}}