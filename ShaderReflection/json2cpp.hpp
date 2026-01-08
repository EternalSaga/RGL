#pragma once

#include <functional>
#include <map>
#include <string>
#include <set>
#include <vector>
#include <nlohmann/json.hpp>

#include <inja/environment.hpp>
#include <inja/inja.hpp>
#include <filesystem>

namespace RGL {
namespace reflection {

    static const std::map<std::string, std::string> GLSL_TYPE_TO_CPP = {
        {"float", "GLfloat"},
        {"vec2", "glm::vec2"},
        {"vec3","glm::vec3"},
        {"vec4", "glm::vec4"},
        {"mat3", "glm::mat3"},
        {"mat4","glm::mat4"},
        {"int", "GLint"},
        {"bool","GLboolean"},
        {"uint","GLuint"}
    };

    class Template2Struct {
    private:
        std::set<int> inputLocations;
        std::set<int> samplerBindings;
        std::set<int> uboBindings;
        std::set<int> ssboBindings;
        
        nlohmann::json shaderJson;

        std::filesystem::path outPutFolder;
        std::filesystem::path shaderName;

        inja::Environment env;


        const std::filesystem::path tempalteHpp = "ShaderReflection/templates/shader_template.hpp.jinja";
        const std::filesystem::path templateCpp = "ShaderReflection/templates/shader_template.cpp.jinja";
    public:
        Template2Struct(const nlohmann::json& j, const std::filesystem::path& outPutFolder,const std::filesystem::path& shaderName);
        void generate();

    };

}}