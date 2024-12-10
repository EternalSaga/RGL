#include "ShaderManager.hpp"
#include "Shader.hpp"
#include "Material.hpp"
#include "Light.hpp"
#include "Entity.hpp"
#include <fmt/format.h>
namespace RGL {
namespace glcore {
ShaderManager *ShaderManager::manager = nullptr;
std::once_flag ShaderManager::initOnce{};
void ShaderManager::refNewShader(const ShaderRef ref, size_t bindingIdx) {
    if (bindingIdxShader.find(ref) != bindingIdxShader.end()) {
	throw std::logic_error("shader has already managed");
    }

    bindingIdxShader[ref] = bindingIdx;
}
void ShaderManager::associate(const std::string &uniformName, const ShaderRef ref) {
    if (bindingIdxShader.find(ref) == bindingIdxShader.end()) {
	throw std::logic_error("shader has not been managed");
    }
    const int idx = bindingIdxShader[ref];
    bindingIdxNames[idx].emplace_back(uniformName);
}
void ShaderManager::associate(const Light &light, const ShaderRef ref) {
    const auto names = light.uniforms();
    for (const auto& name : names) {
		associate(name, ref);
    }
}

void ShaderManager::associate(const Material& material, const ShaderRef ref) {
    const auto names = material.uniformNames();
    for (const auto& name: names) {
	associate(name, ref);
    }
}
void ShaderManager::associate(const CommonEntity &commonEntity, const ShaderRef ref) {
    const auto names = commonEntity.uniforms();
    for (const auto &name : names) {
	associate(name, ref);
    }
}
void ShaderManager::updateUniform(const std::string &uniformName, const CommonUniformTypes &value) {
    uniformValues[uniformName] = value;
}

void ShaderManager::updateAllUnifoms() {
    for (auto [shaderRef, id] : bindingIdxShader) {
	const auto unames = bindingIdxNames[id];
	ScopeShader scopeshader(*shaderRef);
	for (const auto &uname : unames) {
	    const auto &value = uniformValues[uname];
	    boost::variant2::visit([&shaderRef, &uname](const auto &v) {
		shaderRef->setUniform(uname, v);
	    },
		value);
	}
    }
}
void ShaderManager::useAllProgram() {
    for (const auto &[shader, bindIdx] : bindingIdxShader) {
	shader->useProgram();
    }
}
void ShaderManager::disableAllProgram() {
    for (const auto &[shader, bindIdx] : bindingIdxShader) {
	shader->disableProgram();
    }
}





}  // namespace glcore
}  // namespace RGL
