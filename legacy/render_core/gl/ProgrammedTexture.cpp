#include "ProgrammedTexture.hpp"

namespace RGL {
namespace texture {

CheckerBoard::CheckerBoard(int rols, int cols) {
    this->cols = cols;
    this->rols = rols;

    data.resize(rols * cols * 4);
    for (int i = 0; i < rols; i++) {
	for (int j = 0; j < cols; j++) {
	    data[i * cols * 4 + j * 4 + 0] = (i + j) % 2 == 0 ? 255 : 0;
	    data[i * cols * 4 + j * 4 + 1] = (i + j) % 2 == 0 ? 255 : 0;
	    data[i * cols * 4 + j * 4 + 2] = (i + j) % 2 == 0 ? 255 : 0;
	    data[i * cols * 4 + j * 4 + 3] = 255;
	}
    }
}
io::ImgRef CheckerBoard::getTexture() {
    return io::ImgRef{data.data(), rols, cols, 4};
}
}  // namespace texture
}  // namespace RGL