#pragma once
#include <vector>
#include <cstdint>
#include "GLTextures.hpp"
namespace RGL {
namespace texture {
class CheckerBoard {
   private:
    std::vector<std::uint8_t> data;
    int rols;
    int cols;

   public:
    CheckerBoard(int rols, int cols);
    io::ImgRef getTexture();
};
}  // namespace texture

}  // namespace RGL