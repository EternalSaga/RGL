#include <boost/gil.hpp>
#include <boost/gil/extension/io/jpeg.hpp>
#include <boost/gil/extension/io/png.hpp>
#include <filesystem>
namespace bg = boost::gil;
namespace fs = std::filesystem;
class RawImg {
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
		else
		{
			throw std::logic_error("Unsupoorted image format.");
		}

		flippedView = bg::flipped_up_down_view(bg::view(image));

	}

	~RawImg() = default;
	const bg::rgba8_view_t getYFlippedView() const {
		return flippedView;
	}
};
int main() {
	RawImg img("E:\\program\\cpp_proj\\RGL\\assest\\002.jpg");
	bg::rgba8_view_t flipped = img.getYFlippedView();
	std::uint8_t* data = bg::interleaved_view_get_raw_data(flipped);
	return 0;
};
