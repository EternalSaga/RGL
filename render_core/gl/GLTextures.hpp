#pragma once
#include "Helpers.hpp"
#include <cstdint>
#include <filesystem>
#include <glad/glad.h>
#include <memory>
#include <stdexcept>




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
} // namespace io

namespace glcore {
using namespace io;

constexpr std::vector<GLuint> initTUnitRes();

// 没有考虑到纹理单元的归还，暂时先这样
class Texture {
  std::unique_ptr<GLuint[]> textures;

  std::unique_ptr<GLint[]> textureUnitIdx;

  size_t mNumOfTextures;
  static std::vector<GLuint> textureUnitResource;

public:
  Texture(size_t numOfTextrues);

  Texture();

  GLint getTextureUnitID(GLuint textrueID) const;

  GLint getTextureUnitID() const;

  void set(const ImgRef &flippedImg, GLuint textIdx, bool turnOnMipmap);

  void set(const ImgRef &flippedImg, bool turnOnMipmap);

  ~Texture();
};
} // namespace glcore
} // namespace RGL