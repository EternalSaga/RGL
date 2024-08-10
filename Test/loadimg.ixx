module;

#include <boost/gil.hpp>
#include <boost/gil/extension/io/jpeg.hpp>
#include <boost/gil/extension/io/png.hpp>
#include <boost/gil/io/read_image.hpp>
#include <filesystem>
#include <cstdint>
export module loadimg;



namespace bg = boost::gil;
namespace fs = std::filesystem;
export class RawImg {
	bg::rgba8_image_t image;
	bg::rgba8_view_t flippedView;
public:
	RawImg(const fs::path& imagePath) {
		if (imagePath.extension() == ".jpg" || imagePath.extension() == ".jpeg")
		{
			bg::read_and_convert_image(imagePath, image, bg::jpeg_tag());
		}
		else if (imagePath.extension() == ".png") {
			bg::read_and_convert_image(imagePath, image, bg::png_tag());
		}

		flippedView = bg::flipped_up_down_view(bg::view(image));

	}

	~RawImg() = default;
	const bg::rgba8_view_t getYFlippedView() const {

		return flippedView;


	}
};
