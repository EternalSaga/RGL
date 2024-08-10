
module;


#include <cstdint>
#include <glad/glad.h>
#include <filesystem>
#include <stdexcept>
#include "Helpers.hpp"
#include <memory>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <cmath>
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

		
		constexpr std::vector<GLuint> initTUnitRes() {
			std::vector<GLuint> tempUnitRes;
			for (size_t i = 0; i < 16; i++)
			{
				tempUnitRes.emplace_back(15 - i);
			}
			return tempUnitRes;
		}


		//没有考虑到纹理单元的归还，暂时先这样
		export class Texture {
			std::unique_ptr<GLuint[]> textures;

			std::unique_ptr<GLint[]> textureUnitIdx;

			size_t mNumOfTextures;
			static std::vector<GLuint> textureUnitResource;


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

			void set(const ImgRef& flippedImg, GLuint textIdx,bool turnOnMipmap) {

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

				const auto tUnit = textureUnitResource.back();
				textureUnitResource.pop_back();
				glCall(glBindTextureUnit, tUnit, currentTexture);

				//储存下纹理和纹理单元的对应关系
				textureUnitIdx[textIdx] = tUnit;

				if (turnOnMipmap)
				{
					glCall(glTextureParameteri, currentTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					glCall(glTextureParameteri, currentTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
					glCall(glTextureParameteri, currentTexture, GL_TEXTURE_WRAP_S, GL_REPEAT);//u方向
					glCall(glTextureParameteri, currentTexture, GL_TEXTURE_WRAP_T, GL_REPEAT);//v方向

					//根据经验计算mipmap等级，向下取整保证不产生过多的mipmap层级
					const GLint mipmapLevel = static_cast<GLint>(std::floor(std::log2(std::max(flippedImg.width, flippedImg.height))));

					glCall(glTextureStorage2D,currentTexture, mipmapLevel, GL_RGBA8, flippedImg.width, flippedImg.height);//初始化空间，一口气分配所有显存,mipmap层级由上个公式计算
					glCall(glTextureSubImage2D, currentTexture, 0, 0, 0, flippedImg.width, flippedImg.height, GL_RGBA, GL_UNSIGNED_BYTE, flippedImg.imgData);//传输等级0的原始数据

					glCall(glGenerateTextureMipmap, currentTexture);//在当前纹理上开启mipmap，由0层的原始数据计算mipmap,注意放到glTextureSubImage2D之后，先传输数据，再计算mipmap
				}
				else {
					//设置纹理的过滤方式
					glCall(glTextureParameteri, currentTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);//当图片像素小于纹理所需要的像素,使用线性插值
					glCall(glTextureParameteri, currentTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);//当图片像素大于纹理所需要的像素,使用最近邻算法
					//设置纹理的包裹方式
					//当UV坐标超出了0-1的范围，使用什么方式进行纹理包裹
					glCall(glTextureParameteri, currentTexture, GL_TEXTURE_WRAP_S, GL_REPEAT);//u方向
					glCall(glTextureParameteri, currentTexture, GL_TEXTURE_WRAP_T, GL_REPEAT);//v方向
					glCall(glTextureStorage2D, currentTexture, 1, GL_RGBA8, flippedImg.width, flippedImg.height);//初始化空间，一口气分配所有显存,因为不采用mipmap，所以这里分配等级只为1，分配一个空间
					glCall(glTextureSubImage2D, currentTexture, 0, 0, 0, flippedImg.width, flippedImg.height, GL_RGBA, GL_UNSIGNED_BYTE, flippedImg.imgData);//根据mipmap等级初始化或者更新纹理数据
				}

				
			}

			void set(const ImgRef& flippedImg, bool turnOnMipmap) {
				assert(mNumOfTextures == 1);
				set(flippedImg, 0, turnOnMipmap);
			}

		


			~Texture()
			{
				glCall(glDeleteTextures,mNumOfTextures, textures.get());
			}
		};
		std::vector<GLuint> Texture::textureUnitResource = initTUnitRes();
	}
}
