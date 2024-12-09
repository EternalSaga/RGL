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
class Material;
class Light;
using CommonUniformTypes = boost::variant2::variant<GLuint, float, GLint, glm::mat2, glm::mat3, glm::mat4, glm::vec2, glm::vec3, glm::vec4>;

using ShaderRef = std::shared_ptr<Shader>;
struct UniformValue {
    CommonUniformTypes value;
};

class ShaderManager {
    std::unordered_map<std::string, CommonUniformTypes> uniformValues;
    std::unordered_map<size_t, std::vector<std::string>> bindingIdxNames;
    std::unordered_map<ShaderRef, size_t> bindingIdxShader;

    static ShaderManager *manager;
    static std::once_flag initOnce;
    ShaderManager() = default;

   public:
    static ShaderManager *getInstance() {
	std::call_once(initOnce, []() {
	    manager = new ShaderManager();
	});
	return manager;
    }

    void refNewShader(const ShaderRef ref, size_t bindingIdx);
    void associate(const std::string &uniformName, const ShaderRef ref);
    void associate(const Light &light, const ShaderRef ref);
    void associate(const Material &material, const ShaderRef ref);
    void updateUniform(const std::string &uniformName, const CommonUniformTypes &value);

    void updateAllUnifoms();

    void useAllProgram();
    void disableAllProgram();
};
}  // namespace glcore
}  // namespace RGL
