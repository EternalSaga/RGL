
#include "GLTextures.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <cmath>
#include <stb_image.h>



namespace RGL
{
namespace io
{

LoadedImg::LoadedImg(const fs::path &imagePath)
{

    stbi_set_flip_vertically_on_load(true);
    imgData = stbi_load(imagePath.generic_string().c_str(), &width,
	&height, &channels, STBI_rgb_alpha);
}
LoadedImg::operator ImgRef()
{
    return ImgRef(imgData, width, height, channels);
}
LoadedImg::~LoadedImg()
{
    if (imgData) {
	stbi_image_free(imgData);
	imgData = nullptr;
    }
}
} // namespace io
namespace glcore
{

std::once_flag TextUnitResources::initOnce{};
std::shared_ptr<TextUnitResources> TextUnitResources::instance;
std::shared_ptr<TextUnitResources>
TextUnitResources::getInstance()
{
    std::call_once(initOnce, []() {

		instance = std::make_shared<TextUnitResources>();
		});
    return instance;
}
GLuint
TextUnitResources::popUnit()
{
    GLuint tunit = 0;
    if (!textureUnitResource.empty()) {
	tunit = textureUnitResource.back();
	textureUnitResource.pop_back();
    }
    return tunit;
}
TextUnitResources::TextUnitResources()
{
    GLint maxTextureUnits;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxTextureUnits);

    
    for (int i = 0; i < maxTextureUnits; ++i) {
	this->textureUnitResource.push_back(i);
    }
}

Texture::Texture(size_t numOfTextrues)
    : mNumOfTextures(numOfTextrues), currentSetIndex(0), mt(time(0))
{
    
    textures = std::make_unique<GLuint[]>(mNumOfTextures);
    unitsPool = TextUnitResources::getInstance();
    glcore::glCall(glCreateTextures, GL_TEXTURE_2D,
	mNumOfTextures, textures.get());
}
Texture::Texture() : Texture(1) {}

GLint
Texture::useTexture(GLuint textrueOjb)
{
    // 激活纹理单元，纹理单元有16个（0-15）
    // 纹理单元链接了纹理和采样器的对应关系
    // 假设有samplerA,samplerB两个采样器
    // t1,t2,t3三个纹理，那么就可以使用纹理单元映射t1,t2,t3和samplerA,samplerB之间的关系
    // unit1:t1-A
    // unit2:t2-B
    // unit3:t3-A
    // 这些对应关系可以随时修改
    // 又是个辣鸡取名，应该叫做texture_sampler_mapper
    GLint tunit = -1;
    // 检查当前texture是否有对应的纹理单元
	if (texture_textureUnit[textrueOjb] == -1) {// 当前纹理没有纹理单元绑定

		tunit = this->unitsPool->popUnit();
	    // 如果pop出来0，则代表没有资源
		// 严格意义上说，纹理单元不会被耗尽，同一个纹理单元可以重复绑定重复draw
		// 但是为了降低绑定次数，提升性能，所以我们需要设计一个返还策略
		// 当前策略是随机挑选一个幸运unit进行返还
		if (tunit==-1) {
		    std::uniform_int_distribution<> uniform_int_dist(0,  texture_textureUnit.size());
		    int luckyText = uniform_int_dist(mt);
		    const auto textObj = this->textures[luckyText];
		    auto it = texture_textureUnit.find(textObj);
		    if (it != texture_textureUnit.end()) {
				tunit = it->second;
				unitsPool->pushUnit(it->second);
				it->second = -1;
		    } else {
				throw std::invalid_argument("Cannot find texture object");
		    }
		}

	    texture_textureUnit[textrueOjb] = tunit;
		glCall(glBindTextureUnit,tunit, textrueOjb);

    } else {
	    tunit = texture_textureUnit[textrueOjb];
	}

	return tunit;
}

inline GLuint
Texture::findTextureByName(std::string textureName)
{
    const auto it = textrueName_texture.find(textureName);
    if (it == textrueName_texture.end()) {
	throw std::invalid_argument("invalid texture name");
    }
    return it->second;
}

GLint
Texture::useTexture(std::string textureName)
{
    const auto textureObj = findTextureByName(textureName);
    return useTexture(textureObj);
}

void
Texture::set(const ImgRef &flippedImg, std::string textureName,
    bool turnOnMipmap)
{
    if (currentSetIndex >= this->mNumOfTextures) {
	throw std::invalid_argument("This texture objects is full");
    }

    const auto currentTexture = textures[currentSetIndex];
   

	//纹理名对应纹理对象
	this->textrueName_texture[textureName] = currentTexture;

	//暂时不绑定任何纹理单元，所以放个-1
	this->texture_textureUnit[currentTexture] = -1;

    if (turnOnMipmap) {
	glCall(glTextureParameteri, currentTexture,
	    GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glCall(glTextureParameteri, currentTexture,
	    GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
	glCall(glTextureParameteri, currentTexture,
	    GL_TEXTURE_WRAP_S,
	    GL_REPEAT); // u方向
	glCall(glTextureParameteri, currentTexture,
	    GL_TEXTURE_WRAP_T,
	    GL_REPEAT); // v方向

	// 根据经验计算mipmap等级，向下取整保证不产生过多的mipmap层级
	const GLint mipmapLevel =
	    static_cast<GLint>(std::floor(std::log2(
		std::max(flippedImg.width, flippedImg.height))));

	glCall(
	    glTextureStorage2D, currentTexture, mipmapLevel,
	    GL_RGBA8, flippedImg.width,
	    flippedImg
		.height); // 初始化空间，一口气分配所有显存,mipmap层级由上个公式计算
	glCall(glTextureSubImage2D, currentTexture, 0, 0, 0,
	    flippedImg.width, flippedImg.height, GL_RGBA,
	    GL_UNSIGNED_BYTE,
	    flippedImg.imgData); // 传输等级0的原始数据

	glCall(
	    glGenerateTextureMipmap,
	    currentTexture); // 在当前纹理上开启mipmap，由0层的原始数据计算mipmap,注意放到glTextureSubImage2D之后，先传输数据，再计算mipmap
    } else {
	// 设置纹理的过滤方式
	glCall(
	    glTextureParameteri, currentTexture,
	    GL_TEXTURE_MAG_FILTER,
	    GL_LINEAR); // 当图片像素小于纹理所需要的像素,使用线性插值
	glCall(
	    glTextureParameteri, currentTexture,
	    GL_TEXTURE_MIN_FILTER,
	    GL_NEAREST); // 当图片像素大于纹理所需要的像素,使用最近邻算法
	// 设置纹理的包裹方式
	// 当UV坐标超出了0-1的范围，使用什么方式进行纹理包裹
	glCall(glTextureParameteri, currentTexture,
	    GL_TEXTURE_WRAP_S,
	    GL_REPEAT); // u方向
	glCall(glTextureParameteri, currentTexture,
	    GL_TEXTURE_WRAP_T,
	    GL_REPEAT); // v方向
	glCall(
	    glTextureStorage2D, currentTexture, 1, GL_RGBA8,
	    flippedImg.width,
	    flippedImg
		.height); // 初始化空间，一口气分配所有显存,因为不采用mipmap，所以这里分配等级只为1，分配一个空间
	glCall(
	    glTextureSubImage2D, currentTexture, 0, 0, 0,
	    flippedImg.width, flippedImg.height, GL_RGBA,
	    GL_UNSIGNED_BYTE,
	    flippedImg
		.imgData); // 根据mipmap等级初始化或者更新纹理数据
    }
    currentSetIndex++;
}

Texture::~Texture()
{
    glCall(glDeleteTextures, mNumOfTextures, textures.get());
}
} // namespace glcore
} // namespace RGL
