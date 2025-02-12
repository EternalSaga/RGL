#pragma once
#include "GLTextures.hpp"
#include "Helpers.hpp"
#include "rllogger.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>

#include <glad/glad.h>
#include <memory>
#include <mutex>

#include <map>
#include <string>
#include <vector>
#include <ctime>
#include <filesystem>

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

    operator ImgRef();

    ~LoadedImg();
};
}  // namespace io

namespace glcore {
using namespace io;

std::vector<GLuint> initTUnitRes();


constexpr GLint GL_INVLAID_TEXTURE_UNIT = GL_TEXTURE0 - 1;

class TextUnitResources {
    
   public:
    ~TextUnitResources() = default;

    // 如果返回0，则代表没有资源，需要push
    GLuint popUnit();
    void pushUnit(GLuint tunit) {
	textureUnitResource.push_back(tunit);
    }
    static std::shared_ptr<TextUnitResources> getInstance();

    TextUnitResources();
    static std::shared_ptr<TextUnitResources> instance;
    std::vector<GLint> textureUnitResource;
    static std::once_flag initOnce;
    GLint MAX_UNIT_SIZE;
};

enum class TextureUsageType {
    DIFFUSE,
    SPECULAR,
    NORMAL,
    AMBIENT,

};

class Texture {

    friend class TextureCache;


    GLuint texture;

    GLint textureUnit;	// 纹理单元
    std::string textureName;	// 纹理名

    std::shared_ptr<TextUnitResources> unitsPool;

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
    GLint getTextureUnit();

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
   public:
    std::shared_ptr<Texture> getTexture(const fs::path &imagePath,TextureUsageType type);

    std::shared_ptr<Texture> getTexture(const ProgrammedTexture type,bool update = false);
};

}  // namespace glcore
}  // namespace RGL

