#include "Material.hpp"

namespace RGL {

namespace glcore {

void AssetMaterialData::appendTexture(TextureUsageType usage,std::shared_ptr<Texture> texture) {
    textures[usage]=texture;
}
}  // namespace glcore
}  // namespace RGL
