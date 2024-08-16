module;
#include <SDL2/SDL.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>
#include <stdexcept>
export module CPURender;
import apiAbstractor;
import SDLWindow;
import GLCheckError;
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
					auto logger = glcore::Logger::getInstance();
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

			operator SDL_Surface*() { return surface_; }
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

		typedef PureElementImage<uint8_t> ColorAttachment;
		typedef PureElementImage<float> DepthAttachment;


		export class SoftwareRenderContext : public RendererContext {

			void* framebuffer;

			std::shared_ptr<SDLWindow> sdlWindow;

			SDL_Renderer* sdlRenderer;
			int width;
			int height;
			ColorAttachment colorSurface;
			void drawpixel() {
				for (size_t i = 0; i < colorSurface.width; i++)
				{
					for (size_t j = 0; j < colorSurface.height; j++) {
						if (i%2==0 && j%2==0)
						{
							auto color = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
							colorSurface.Set(i, j, color);
						}
					}
				}
			}

		public:
			void render() override {
				auto clearColor = glm::vec4(0.2f, 0.2f, 0.2f, 1.0f);
				colorSurface.Clear(clearColor);
				drawpixel();
				SwapBuffer(colorSurface);
				//(*renderer)();
			}
			void SwapBuffer(SDL_Surface* surface) {
				SDL_Texture* texture = SDL_CreateTextureFromSurface(sdlRenderer, surface);
				// 画面
				if (!texture) {
					SDL_Log("swap buffer failed");
				}
				else {
					SDL_RenderCopy(sdlRenderer, texture, nullptr, nullptr);
					SDL_DestroyTexture(texture);
				}

				SDL_RenderPresent(sdlRenderer);
			}

			SoftwareRenderContext(std::shared_ptr<SDLWindow> window):sdlWindow(window), colorSurface(window->getWidth(),window->getHeight()){
				
				sdlRenderer = SDL_CreateRenderer(*sdlWindow, -1, SDL_RENDERER_SOFTWARE);

				if (!sdlRenderer)
				{
					throw std::runtime_error("cannot create renderer");
				}

				

			}

			~SoftwareRenderContext() {
				SDL_DestroyRenderer(sdlRenderer);
			}
		};


	}
}
