
module;


#include <cstdint>
#include <glad/glad.h>
#include <filesystem>
#include <stdexcept>
#include "Helpers.hpp"
#include <memory>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
export module GLTexture;


namespace RGL {
	namespace io {
		namespace fs = std::filesystem;
		export struct ImgRef {
			uint8_t* imgData;
			int width;
			int height;
			int channels;
		};
		export class LoadedImg {
			uint8_t* imgData;
			int width;
			int height;
			int channels;
		public:
			LoadedImg(const fs::path& imagePath) {

				stbi_set_flip_vertically_on_load(true);
				imgData = stbi_load(imagePath.generic_string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
			}

			operator ImgRef() {
				return ImgRef(imgData, width, height, channels);
			}

			~LoadedImg() {
				if (imgData)
				{
					stbi_image_free(imgData);
					imgData = nullptr;
				}
			}
		};
	}
	namespace glcore {
		using namespace io;

		


		export class Texture {
			std::unique_ptr<GLuint[]> textures;

			std::unique_ptr<GLint[]> textureUnitIdx;

			size_t mNumOfTextures;

		public:

			Texture(size_t numOfTextrues) :mNumOfTextures(numOfTextrues) {
				textures = std::make_unique<GLuint[]>(mNumOfTextures);
				textureUnitIdx = std::make_unique<GLint[]>(mNumOfTextures);
				glcore::glCall(glCreateTextures, GL_TEXTURE_2D, mNumOfTextures, textures.get());

			}

			Texture() :Texture(1) {
			}

			GLint getTextureUnitID(GLuint textrueID) const {
				assert(textrueID < mNumOfTextures);
				return textureUnitIdx[textrueID];
			}

			GLint getTextureUnitID() const {
				return getTextureUnitID(0);
			}

			void set(const ImgRef& flippedImg, GLuint textIdx, GLuint tUnitIdx,GLuint mipmapLevel) {

				const auto& currentTexture = textures[textIdx];
				//激活纹理单元，纹理单元有16个（0-15）
				//纹理单元链接了纹理和采样器的对应关系
				//假设有samplerA,samplerB两个采样器
				//t1,t2,t3三个纹理，那么就可以使用纹理单元映射t1,t2,t3和samplerA,samplerB之间的关系
				//unit1:t1-A
				//unit2:t2-B
				//unit3:t3-A
				//这些对应关系可以随时修改
				//又是个辣鸡取名，应该叫做texture_sampler_mapper
				assert(tUnitIdx < 32);
				glCall(glBindTextureUnit,tUnitIdx, currentTexture);

				//储存下纹理和纹理单元的对应关系
				textureUnitIdx[textIdx] = tUnitIdx;

				//设置纹理的过滤方式
				glCall(glTextureParameteri,currentTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);//当图片像素小于纹理所需要的像素,使用线性插值
				glCall(glTextureParameteri,currentTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);//当图片像素大于纹理所需要的像素,使用最近邻算法
				//设置纹理的包裹方式
				//当UV坐标超出了0-1的范围，使用什么方式进行纹理包裹
				glCall(glTextureParameteri,currentTexture, GL_TEXTURE_WRAP_S, GL_REPEAT);//u方向
				glCall(glTextureParameteri,currentTexture, GL_TEXTURE_WRAP_T, GL_REPEAT);//v方向
				glCall(glTextureStorage2D, currentTexture, 1, GL_RGBA8, flippedImg.width, flippedImg.height);//分配储存空间
				glCall(glTextureSubImage2D,currentTexture, mipmapLevel, 0, 0, flippedImg.width, flippedImg.height, GL_RGBA, GL_UNSIGNED_BYTE, flippedImg.imgData);//根据mipmap等级传输数据
			}

			void set(const ImgRef& flippedImg,GLuint mipmapLevel) {
				assert(mNumOfTextures == 1);
				set(flippedImg, 0, 0, mipmapLevel);
			}

			~Texture()
			{
				glCall(glDeleteTextures,mNumOfTextures, textures.get());
			}
		};
	}
}
