#pragma once
#include "Helpers.hpp"
#include <cstdint>
#include <filesystem>
#include <glad/glad.h>
#include <memory>
#include <mutex>

#include <map>
#include <vector>
#include <random>
#include <ctime>
namespace RGL
{
namespace io
{
namespace fs = std::filesystem;
struct ImgRef {
    uint8_t *imgData;
    int width;
    int height;
    int channels;
};
class LoadedImg
{
    uint8_t *imgData;
    int width;
    int height;
    int channels;

  public:
    LoadedImg(const fs::path &imagePath);

    operator ImgRef();

    ~LoadedImg();
};
} // namespace io

namespace glcore
{
using namespace io;

std::vector<GLuint> initTUnitRes();

class TextUnitResources 
{
  public:
    ~TextUnitResources() = default;
    
	//如果返回0，则代表没有资源，需要push
    GLuint popUnit();
	void pushUnit(GLuint tunit) {
	    textureUnitResource.push_back(tunit);
	}
	static std::shared_ptr<TextUnitResources> getInstance();

    TextUnitResources();
    static std::shared_ptr<TextUnitResources> instance;
    std::vector<GLint> textureUnitResource;
    static std::once_flag initOnce;
};

// 没有考虑到纹理单元的归还，暂时先这样
class Texture
{
	std::mt19937 mt;//随机选择一个幸运纹理单元被重新绑定
    

    std::unique_ptr<GLuint[]> textures;
    size_t currentSetIndex;
    std::map<GLuint, GLint> texture_textureUnit;      // 纹理-纹理单元映射
    std::map<std::string, GLuint> textrueName_texture; // 纹理名-纹理
    size_t mNumOfTextures;
    std::shared_ptr<TextUnitResources> unitsPool;
  public:
    Texture(size_t numOfTextrues);

    Texture();
    GLint useTexture(GLuint textrueID);
    GLuint findTextureByName(std::string textureName);


	GLint useTexture(std::string textureName);

    void set(const ImgRef &flippedImg, std::string textureName, bool turnOnMipmap);

    ~Texture();
};
} // namespace glcore
} // namespace RGL
;
