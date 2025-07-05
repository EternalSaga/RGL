#pragma once
#include "GLTextures.hpp"
#include "Helpers.hpp"
#include "rllogger.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>

#include <assimp/material.h>
#include <glad/glad.h>
#include <memory>
#include <mutex>

#include <map>
#include <string>
#include <vector>
#include <ctime>
#include <filesystem>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>

namespace RGL {
namespace io {
namespace fs = std::filesystem;
struct ImgRef {
    uint8_t *imgData;
    int width;
    int height;
    int channels;
};
class LoadedImg {
    uint8_t *imgData;
    int width;
    int height;
    int channels;

   public:
    LoadedImg(const fs::path &imagePath);
    LoadedImg(const aiTexture* embededtexture);

    operator ImgRef();

    ~LoadedImg();
};
}  // namespace io

namespace glcore {
using namespace io;

std::vector<GLuint> initTUnitRes();




enum class TextureUsageType {
    DIFFUSE = 1,
    SPECULAR = 2,
    NORMAL = 3,
    AMBIENT = 4
};

class Texture {

    friend class TextureCache;


    GLuint texture;

    GLuint64 textureHandle;	// 纹理句柄

    std::string textureName;	// 纹理名


    TextureUsageType usageType;
    void setTextureUnit();
   public:
    Texture();
    void useTexture();
    inline std::string getName(){
        if (textureName.empty()) {
            throw std::runtime_error("Texture name is not set.");
        }
        return textureName;
    }
    void disableTexture();
    inline GLuint64 getTextureHandler() const { return textureHandle; }

    void set(const ImgRef &flippedImg, bool turnOnMipmap);

    void setUseType(TextureUsageType type);
    void setName(std::string name){
        textureName = name;
    }
    TextureUsageType getUseType();
    // 谨慎使用，用之前知道自己在干什么
    void setFilltering( GLenum filter);

    GLuint operator()();

    ~Texture();
};


enum class ProgrammedTexture{
    CHECKERBOARD,
    NOISE,
    WOOD,
    MARBLE
};

class TextureCache {
    std::map<fs::path, std::shared_ptr<Texture>> cache;
    std::map<ProgrammedTexture, std::shared_ptr<Texture>> programmedTexturesCache;
    std::map<const aiTexture*, std::shared_ptr<Texture>> aiCache;
   public:
    std::shared_ptr<Texture> getTexture(const fs::path &imagePath,TextureUsageType type);

    std::shared_ptr<Texture> getTexture(const ProgrammedTexture type,bool update = false);

    std::shared_ptr<Texture> getTexture(const aiTexture* texture,TextureUsageType type);

};

}  // namespace glcore
}  // namespace RGL

