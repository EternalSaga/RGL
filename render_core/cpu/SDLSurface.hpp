#pragma once
#include <SDL2/SDL.h>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
#include "rllogger.hpp"
namespace RGL {
	namespace swr {
		template <typename T>
		class PureElementImage {
		public:
			std::vector<T> data;
			uint32_t width;
			uint32_t height;
			PureElementImage(std::vector<T> data, uint32_t width, uint32_t height)
				: data(data), width(width), height(height) {}
		};


		template <>
		class PureElementImage<uint8_t> {
		private:
			SDL_Surface* surface_;

			Uint32* getPixel(int x, int y) const {
				Uint8* ptr = (Uint8*)surface_->pixels;
				return (Uint32*)(ptr + y * surface_->pitch +
					x * surface_->format->BytesPerPixel);
			}

		public:
			std::vector<uint8_t> data;
			uint32_t width;
			uint32_t height;

			PureElementImage(std::vector<uint8_t> data, uint32_t width, uint32_t height)
				: data(data), width(width), height(height) {
				surface_ = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
					SDL_PIXELFORMAT_RGBA32);
				if (!surface_) {
					auto logger = RGL::RLLogger::getInstance();
					logger->error("Create Surface failed: {}", SDL_GetError());
				}
			}

			PureElementImage(uint32_t w, uint32_t h)
				: PureElementImage<uint8_t>(std::vector<uint8_t>(w* h * 4, 0), w, h) {}

			PureElementImage(const PureElementImage&) = delete;

			~PureElementImage() { SDL_FreeSurface(surface_); }

			void Set(uint32_t x, uint32_t y, glm::vec4& color) {
				data[(x + y * width) * 4] = color.r * 255.0;
				data[(x + y * width) * 4 + 1] = color.g * 255.0;
				data[(x + y * width) * 4 + 2] = color.b * 255.0;
				data[(x + y * width) * 4 + 3] = color.a * 255.0;
				*getPixel(x, y) =
					SDL_MapRGBA(surface_->format, color.r * 255.0, color.g * 255.0,
						color.b * 255.0, color.a * 255.0);
			}

			void Clear(glm::vec4& color) {
				for (int i = 0; i < width; i++) {
					for (int j = 0; j < height; j++) {
						Set(i, j, color);
					}
				}
				SDL_FillRect(surface_, nullptr,
					SDL_MapRGBA(surface_->format, color.r * 255, color.g * 255,
						color.b * 255, color.a * 255));
			}

			operator SDL_Surface* () { return surface_; }
		};

		template <>
		class PureElementImage<float> {
		public:
			std::vector<float> data;
			uint32_t width;
			uint32_t height;

			PureElementImage(std::vector<float> data, uint32_t width, uint32_t height)
				: data(data), width(width), height(height) {}

			PureElementImage(uint32_t w, uint32_t h)
				: PureElementImage(std::vector<float>(w* h, FLT_MIN), w, h) {}

			void Set(uint32_t x, uint32_t y, float value) {
				data[x + y * width] = value;
			}

			float Get(uint32_t x, uint32_t y) { return data[x + y * width]; }

			void Clear(float value) {
				for (int i = 0; i < width; i++) {
					for (int j = 0; j < height; j++) {
						Set(i, j, value);
					}
				}
			}
		};

		using ColorAttachment = PureElementImage<uint8_t>;

		using DepthAttachment = PureElementImage<float>;

	}
}

