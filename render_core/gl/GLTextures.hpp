#pragma once
#include "GLTextures.hpp"
#include "Helpers.hpp"

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


enum class TextureUsageType {
    BASE_COLOR = 0,
    NORMAL = 1,
    EMISSIVE = 2,
    PACKED_ORM = 3,// Occlusion（Ambient Occlusion）, Roughness, Metalness
    SPECULAR = 4
};

std::string TextureType2Str(const TextureUsageType &usageType);



class Texture {

    friend class TextureCache;


    GLuint texture;



   public:
    Texture();

    void set(const ImgRef &flippedImg, bool turnOnMipmap);


    // 谨慎使用，用之前知道自己在干什么
    void setFilltering( GLenum filter);

    inline operator GLuint() const{
        return texture;
    }

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
    std::shared_ptr<Texture> getTexture(const fs::path &imagePath);

    std::shared_ptr<Texture> getTexture(const ProgrammedTexture type,bool update = false);

    std::shared_ptr<Texture> getTexture(const aiTexture* texture);

};

class PassTextureManager{
    std::map<GLuint, GLint> textureToUnitMap;
	std::vector<std::shared_ptr<Texture>> activeTextures;
	GLint nextUnit = 0;

	GLint maxTextureUnits;


	public:
	PassTextureManager(){
		glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxTextureUnits);
	}

	GLint assignUnit(const std::shared_ptr<Texture>& texture);


	std::vector<GLint> bindAll();
};




}  // namespace glcore
}  // namespace RGL

