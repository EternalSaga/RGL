#include "ShaderManager.hpp"
#include "Shader.hpp"
#include "Material.hpp"
#include "Light.hpp"
#include "Entity.hpp"
#include <fmt/format.h>
namespace RGL {
namespace glcore {


void updateAllUniforms(ShaderRef shader, const DiscreteUniforms &uniforms) {
    for (const auto& [uname,value] : uniforms) {
	boost::variant2::visit([&shader, &uname](const auto &v) {
	    shader->setUniform(uname, v);
	},
	    value);
    }
}

}  // namespace glcore
}  // namespace RGL
