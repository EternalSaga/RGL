module;
#include <SDL2/SDL.h>
#include <vector>
#include <cstdint>
export module SoftwareRenderPractice;
import apiAbstractor;
namespace RGL {
	namespace swr {
		void draw_rectangle(SDL_Surface* surface, int x, int y, int width, int height)
		{
			SDL_LockSurface(surface);
			std::vector<uint8_t> pixels(surface->h * surface->pitch, 0);

			int dy, dx;
			int maxwidth = width * 3;
			for (dy = y; dy < height; dy++) {
				for (dx = x; dx < maxwidth; dx += 3) {
					pixels[dx + (dy * surface->pitch)] = 0;
					pixels[dx + (dy * surface->pitch) + 1] = 255;
					pixels[dx + (dy * surface->pitch) + 2] = 0;
				}
			}
			memcpy(surface->pixels, pixels.data(), surface->pitch * surface->h);

			SDL_UnlockSurface(surface);
		}
		export class TestCPURender : public Renderer {
			SDL_Surface* surface;
			int width;
			int height;

		public:
			TestCPURender(SDL_Surface* surface):surface(surface) {
				width = surface->w;
				height = surface->h;
			}
			void operator()() override {
				draw_rectangle(surface, 0, 0, 500, 500);
			}
		};
	}
}
