
#include "GLTextures.hpp"
#include "GLCheckError.hpp"
#include "Helpers.hpp"
#include "rllogger.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <cmath>
#include <stb_image.h>

#include "ProgrammedTexture.hpp"
namespace RGL {
namespace io {

LoadedImg::LoadedImg(const fs::path &imagePath) {
    stbi_set_flip_vertically_on_load(true);
    imgData = stbi_load(imagePath.generic_string().c_str(), &width,
	&height, &channels, STBI_rgb_alpha);
	if (imgData==nullptr) {
		auto logger = RLLogger::getInstance();
		logger->error("Failed to load image: {}", imagePath.generic_string());
		throw std::runtime_error("Failed to load image");
	}
}
LoadedImg::operator ImgRef() {
    return ImgRef(imgData, width, height, channels);
}
LoadedImg::~LoadedImg() {
    if (imgData) {
	stbi_image_free(imgData);
	imgData = nullptr;
    }
}
}  // namespace io
namespace glcore {

std::once_flag TextUnitResources::initOnce{};
std::shared_ptr<TextUnitResources> TextUnitResources::instance;
std::shared_ptr<TextUnitResources>
TextUnitResources::getInstance() {
    std::call_once(initOnce, []() {
	instance = std::make_shared<TextUnitResources>();
    });
    return instance;
}
GLuint
TextUnitResources::popUnit() {
    GLuint tunit = 0;
    if (!textureUnitResource.empty()) {
	tunit = textureUnitResource.back();
	textureUnitResource.pop_back();
    } else {
	tunit = GL_INVLAID_TEXTURE_UNIT;  // 没有可用的纹理单元，返回-1
    }
    return tunit;
}
TextUnitResources::TextUnitResources() {
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &MAX_UNIT_SIZE);
    textureUnitResource.reserve(MAX_UNIT_SIZE);
    for (int i = 0; i < MAX_UNIT_SIZE; ++i) {
	this->textureUnitResource.push_back(i);
    }
}

Texture::Texture() {
    unitsPool = TextUnitResources::getInstance();
    glcore::glCall(glCreateTextures, GL_TEXTURE_2D,
	1, &texture);
}

void Texture::useTexture() {

	setTextureUnit();
    glCall(glBindTextureUnit, textureUnit, texture);
}

void Texture::setTextureUnit(){
    // 检查当前texture是否有对应的纹理单元
    textureUnit = this->unitsPool->popUnit();
    if (textureUnit == GL_INVLAID_TEXTURE_UNIT) {  // 当前纹理没有纹理单元绑定
	auto logger = RLLogger::getInstance();
	logger->error("no texture unit for this texture {}", this->textureName);
	throw std::runtime_error("no texture unit for this texture");
    }

}

void Texture::disableTexture() {
    if (textureUnit != GL_INVLAID_TEXTURE_UNIT) {  // 检查是否分配了纹理单元
	unitsPool->pushUnit(textureUnit);
	textureUnit = GL_INVLAID_TEXTURE_UNIT;  // 重置纹理单元
    }
}

void Texture::set(const ImgRef &flippedImg,
    bool turnOnMipmap) {
    // 暂时不绑定任何纹理单元，所以放个-1
    textureUnit = GL_INVLAID_TEXTURE_UNIT;

    if (turnOnMipmap) {
	glCall(glTextureParameteri, texture,
	    GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glCall(glTextureParameteri, texture,
	    GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
	glCall(glTextureParameteri, texture,
	    GL_TEXTURE_WRAP_S,
	    GL_REPEAT);	 // u方向
	glCall(glTextureParameteri, texture,
	    GL_TEXTURE_WRAP_T,
	    GL_REPEAT);	 // v方向

	// 根据经验计算mipmap等级，向下取整保证不产生过多的mipmap层级
	const GLint mipmapLevel =
	    static_cast<GLint>(std::floor(std::log2(
		std::max(flippedImg.width, flippedImg.height))));

	glCall(
	    glTextureStorage2D, texture, mipmapLevel,
	    GL_RGBA8, flippedImg.width,
	    flippedImg
		.height);  // 初始化空间，一口气分配所有显存,mipmap层级由上个公式计算
	glCall(glTextureSubImage2D, texture, 0, 0, 0,
	    flippedImg.width, flippedImg.height, GL_RGBA,
	    GL_UNSIGNED_BYTE,
	    flippedImg.imgData);  // 传输等级0的原始数据

	glCall(
	    glGenerateTextureMipmap,
	    texture);  // 在当前纹理上开启mipmap，由0层的原始数据计算mipmap,注意放到glTextureSubImage2D之后，先传输数据，再计算mipmap
    } else {
	// 设置纹理的过滤方式
	glCall(
	    glTextureParameteri, texture,
	    GL_TEXTURE_MAG_FILTER,
	    GL_LINEAR);	 // 当图片像素小于纹理所需要的像素,使用线性插值
	glCall(
	    glTextureParameteri, texture,
	    GL_TEXTURE_MIN_FILTER,
	    GL_NEAREST);  // 当图片像素大于纹理所需要的像素,使用最近邻算法
	// 设置纹理的包裹方式
	// 当UV坐标超出了0-1的范围，使用什么方式进行纹理包裹
	glCall(glTextureParameteri, texture,
	    GL_TEXTURE_WRAP_S,
	    GL_REPEAT);	 // u方向
	glCall(glTextureParameteri, texture,
	    GL_TEXTURE_WRAP_T,
	    GL_REPEAT);	 // v方向
	glCall(
	    glTextureStorage2D, texture, 1, GL_RGBA8,
	    flippedImg.width,
	    flippedImg
		.height);  // 初始化空间，一口气分配所有显存,因为不采用mipmap，所以这里分配等级只为1，分配一个空间
	glCall(
	    glTextureSubImage2D, texture, 0, 0, 0,
	    flippedImg.width, flippedImg.height, GL_RGBA,
	    GL_UNSIGNED_BYTE,
	    flippedImg
		.imgData);  // 根据mipmap等级初始化或者更新纹理数据
    }
}

void Texture::setFilltering(GLenum filter) {
    glCall(glTextureParameteri, texture, GL_TEXTURE_MAG_FILTER, filter);
    glCall(glTextureParameteri, texture, GL_TEXTURE_MIN_FILTER, filter);
}

Texture::~Texture() {
    glCall(glDeleteTextures, 1, &texture);
}

void Texture::setUseType(TextureUsageType type) {
    usageType = type;
}
std::shared_ptr<Texture> TextureCache::getTexture(const fs::path &imagePath, TextureUsageType type) {
    if (cache.find(imagePath) == cache.end()) {
	cache[imagePath] = std::make_shared<Texture>();
	cache[imagePath]->set(LoadedImg(imagePath),true);
	cache[imagePath]->setUseType(type);
	switch (type) {
		case TextureUsageType::DIFFUSE:{
			cache[imagePath]->setName("baseColorTexture");
			break;
		}
		case TextureUsageType::SPECULAR:{
			cache[imagePath]->setName("specularTexture");
			break;
		}
		case TextureUsageType::NORMAL:{
			cache[imagePath]->setName("normalTexture");
			break;
		}
		case TextureUsageType::AMBIENT:{
			cache[imagePath]->setName("ambentTexture");
			break;
		}
		default:{
			auto logger = RLLogger::getInstance();
			logger->error("texture usage type not supported");
			throw std::runtime_error("texture usage type not supported");
			break;	
		}
	}

    } else {
	auto logger = RLLogger::getInstance();
	logger->trace("texture cache hit");
    }

    if (cache[imagePath]->usageType != type) {
	auto logger = RLLogger::getInstance();
	logger->warn("texture usage type changed");
	cache[imagePath]->setUseType(type);
    }

    return cache[imagePath];
}

TextureUsageType Texture::getUseType() {
    return usageType;
}
std::shared_ptr<Texture> TextureCache::getTexture(const ProgrammedTexture type,bool update) {
    if (programmedTexturesCache.find(type) == programmedTexturesCache.end() || update) {
		switch(type){
	    case ProgrammedTexture::CHECKERBOARD:{
			programmedTexturesCache[type] = std::make_shared<Texture>();
			texture::CheckerBoard checkerboard{8,8};
			programmedTexturesCache[type]->set(checkerboard.getTexture(), true);
			programmedTexturesCache[type]->setUseType(TextureUsageType::DIFFUSE);
			programmedTexturesCache[type]->setName("baseColorTexture");
			break;
		}
		default:
		auto logger = RLLogger::getInstance();
		logger->error("programmed texture type not supported");
			throw std::runtime_error("programmed texture type not supported");
			break;	
	}

	return programmedTexturesCache[type];
}
}
GLuint Texture::operator()() {
    return texture;
}
GLint Texture::getTextureUnit() {
    if (textureUnit == GL_INVLAID_TEXTURE_UNIT) {
	throw std::runtime_error("Texture unit is not set.");
    }
    if (textureUnit < 0 || textureUnit >= unitsPool->MAX_UNIT_SIZE) {
	throw std::runtime_error("Invalid texture unit.");
    }
    return textureUnit;
}
}  // namespace glcore
}  // namespace RGL
