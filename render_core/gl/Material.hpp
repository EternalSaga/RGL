#include <cassert>

#pragma once


#include <GLTextures.hpp>
#include "ShaderManager.hpp"
namespace RGL {

namespace glcore {


class MaterialData {
    std::map<TextureUsageType,std::vector<std::shared_ptr<Texture>>> textures;
    public:
    MaterialData() = default;
    void appendTexture(std::shared_ptr<Texture> texture);
    inline auto getTextures()->decltype(auto){
        return textures;
    }
};
}  // namespace glcore

}  // namespace RGL
