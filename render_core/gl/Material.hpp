#include <cassert>

#pragma once


#include <GLTextures.hpp>
#include "ShaderManager.hpp"
namespace RGL {

namespace glcore {


class MaterialData {
    std::map<TextureUsageType,std::vector<std::shared_ptr<Texture>>> textures;
    bool isTransparent = false;
    public:
    MaterialData() = default;
    void appendTexture(std::shared_ptr<Texture> texture);
    inline auto getTextures()->decltype(auto){
        return textures;
    }

    inline void setTransparent(bool isTransparent) {
        isTransparent = isTransparent;
    }

    inline bool getTransparent() const {
        return isTransparent;
    }
};
}  // namespace glcore

}  // namespace RGL
