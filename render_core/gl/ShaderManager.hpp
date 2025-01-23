#pragma once
#include <boost/variant2.hpp>
#include <glad/glad.h>

#include <glm/glm.hpp>
#include <unordered_map>
#include <memory>
#include <mutex>
#include "Shader.hpp"

namespace RGL {
namespace glcore {
using ShaderRef = std::shared_ptr<Shader>;

using CommonUniformTypes = boost::variant2::variant<GLuint, float, GLint, glm::mat2, glm::mat3, glm::mat4, glm::vec2, glm::vec3, glm::vec4>;
using DiscreteUniforms = std::unordered_map<std::string, CommonUniformTypes>;


void updateAllUniforms(ShaderRef shader,const DiscreteUniforms& uniforms);

}  // namespace glcore
}  // namespace RGL
    