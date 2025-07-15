#include "Material.hpp"

namespace RGL {

namespace glcore {

void AssetMaterialData::appendTexture(std::shared_ptr<Texture> texture) {
    textures[texture->getUseType()]=texture;
}
}  // namespace glcore
}  // namespace RGL
