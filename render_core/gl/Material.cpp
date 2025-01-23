#include "Material.hpp"
#include "rllogger.hpp"
#include "ShaderManager.hpp"
namespace RGL {

namespace glcore {

void MaterialData::appendTexture(std::shared_ptr<Texture> texture) {
    textures[texture->getUseType()].push_back(texture);
}
}  // namespace glcore
}  // namespace RGL
