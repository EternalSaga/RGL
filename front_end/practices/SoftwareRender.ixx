module;
#include <SDL2/SDL.h>
#include <vector>
#include <cstdint>
export module SoftwareRenderPractice;
import apiAbstractor;
namespace RGL {
	namespace swr {
		void set_pixel(SDL_Surface* surface, int x, int y, Uint32 pixel)
		{
			Uint32* const target_pixel = (Uint32*)((Uint8*)surface->pixels
				+ y * surface->pitch
				+ x * surface->format->BytesPerPixel);
			*target_pixel = pixel;
		}
		export class TestCPURender : public Renderer {
			SDL_Surface* surface;
			int width;
			int height;
			SDL_Renderer* sdlRenderer;
		public:
			TestCPURender(SDL_Surface* surface):surface(surface) {
				width = surface->w;
				height = surface->h;
				
			}
			void operator()() override {
				auto red = SDL_MapRGBA(surface->format, 255, 0, 0,0);
				SDL_LockSurface(surface);
				for (size_t i = 0; i < width; i++)
				{
					for (size_t j = 0; j < height; j++)
					{
						set_pixel(surface, i, j, red);
					}
				}
				SDL_UnlockSurface(surface);
			}

		};
	}
}
