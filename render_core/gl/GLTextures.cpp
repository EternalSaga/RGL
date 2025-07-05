
#include "GLTextures.hpp"
#include <glad/glad.h>
#include <cstdint>
#include "GLCheckError.hpp"
#include "Helpers.hpp"
#include "rllogger.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <cmath>
#include <stb_image.h>

#include "ProgrammedTexture.hpp"
namespace RGL {
namespace io {

LoadedImg::LoadedImg(const fs::path& imagePath):imgData(nullptr) {
    stbi_set_flip_vertically_on_load(true);
    imgData = stbi_load(imagePath.generic_string().c_str(), &width,
	&height, &channels, STBI_rgb_alpha);
    if (imgData == nullptr) {
	auto logger = RLLogger::getInstance();
	logger->error("Failed to load image: {}", imagePath.generic_string());
	throw std::runtime_error("Failed to load image");
    }
}

LoadedImg::LoadedImg(const aiTexture* embededtexture):imgData(nullptr) {
    const uint8_t* buffer = reinterpret_cast<uint8_t*>(embededtexture->pcData);
    stbi_set_flip_vertically_on_load(true);
    this->imgData = stbi_load_from_memory(buffer, embededtexture->mWidth, &this->width, &this->height, &channels, STBI_rgb_alpha);
    if (imgData == nullptr) {
	auto logger = RLLogger::getInstance();
	logger->error("Failed to load embedded image");
	throw std::runtime_error("Failed to load embedded image");
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


Texture::Texture() {
        glcore::glCall(glCreateTextures, GL_TEXTURE_2D,
	1, &texture);
}


void Texture::useTexture() {
    glCall(glMakeTextureHandleResidentARB, textureHandle);
}
void Texture::disableTexture() {
	glCall(glMakeTextureHandleNonResidentARB, textureHandle);

}

void Texture::set(const ImgRef& flippedImg,
    bool turnOnMipmap) {


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

	textureHandle = glCall(glGetTextureHandleARB,texture);
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
std::shared_ptr<Texture> TextureCache::getTexture(const fs::path& imagePath, TextureUsageType type) {
    if (cache.find(imagePath) == cache.end()) {
	cache[imagePath] = std::make_shared<Texture>();
	cache[imagePath]->set(LoadedImg(imagePath), true);
	cache[imagePath]->setUseType(type);
	switch (type) {
	case TextureUsageType::DIFFUSE: {
	    cache[imagePath]->setName("baseColorTexture");
	    break;
	}
	case TextureUsageType::SPECULAR: {
	    cache[imagePath]->setName("specularTexture");
	    break;
	}
	case TextureUsageType::NORMAL: {
	    cache[imagePath]->setName("normalTexture");
	    break;
	}
	case TextureUsageType::AMBIENT: {
	    cache[imagePath]->setName("ambentTexture");
	    break;
	}
	default: {
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
std::shared_ptr<Texture> TextureCache::getTexture(const aiTexture* texture, TextureUsageType type) {
    auto logger = RLLogger::getInstance();
    if (aiCache.find(texture) != aiCache.end()) {
	logger->trace("ai texture cache hit");
	return aiCache[texture];
    } else {
	logger->trace("ai texture cache miss");
	aiCache[texture] = std::make_shared<Texture>();
	
	aiCache[texture]->set(LoadedImg(texture), true);
	aiCache[texture]->setUseType(type);
	switch (type) {
	case TextureUsageType::DIFFUSE: {
	    aiCache[texture]->setName("baseColorTexture");
	    break;
	}
	case TextureUsageType::SPECULAR: {
	    aiCache[texture]->setName("specularTexture");
	    break;
	}
	case TextureUsageType::NORMAL: {
	    aiCache[texture]->setName("normalTexture");
	    break;
	}
	case TextureUsageType::AMBIENT: {
	    aiCache[texture]->setName("ambentTexture");
	    break;
	}
	default: {
	    auto logger = RLLogger::getInstance();
	    logger->error("texture usage type not supported");
	    throw std::runtime_error("texture usage type not supported");
	    break;
	}
	}

	return aiCache[texture];

    }
}
TextureUsageType Texture::getUseType() {
    return usageType;
}
std::shared_ptr<Texture> TextureCache::getTexture(const ProgrammedTexture type, bool update) {
    if (programmedTexturesCache.find(type) == programmedTexturesCache.end() || update) {
	switch (type) {
	case ProgrammedTexture::CHECKERBOARD: {
	    programmedTexturesCache[type] = std::make_shared<Texture>();
	    texture::CheckerBoard checkerboard{8, 8};
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
    } else {
	auto logger = RLLogger::getInstance();
	logger->trace("programmed texture cache hit");
	return programmedTexturesCache[type];
    }
}
GLuint Texture::operator()() {
    return texture;
}

}  // namespace glcore
}  // namespace RGL
