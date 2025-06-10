#include <cassert>
#include <glm/fwd.hpp>

#pragma once

#include <GLTextures.hpp>
#include "ShaderManager.hpp"
namespace RGL {

namespace glcore {

struct PBRComponent {
    glm::vec4 baseColorFactor = glm::vec4(0.7f, 0.7f, 0.7f, 1.0f);
    float roughnessFactor = 0.5f;
    float metallicFactor = 0.5f;
    bool isEmpty = true;
    PBRComponent() = default;
    PBRComponent(glm::vec4 baseColorFactor, float roughnessFactor, float metallicFactor)
	: baseColorFactor(baseColorFactor), roughnessFactor(roughnessFactor), metallicFactor(metallicFactor) {}
};

class MaterialData {
    std::map<TextureUsageType, std::vector<std::shared_ptr<Texture>>> textures;
    bool isTransparent = false;

   public:
    MaterialData() = default;
    void appendTexture(std::shared_ptr<Texture> texture);
    inline auto getTextures() -> decltype(auto) {
	return textures;
    }

    inline void setTransparent(bool isTransparent) {
	this->isTransparent = isTransparent;
    }

    inline bool ifHasTextures() const {
	return !textures.empty();
    }

    inline bool getTransparent() const {
	return isTransparent;
    }
};
}  // namespace glcore

}  // namespace RGL
