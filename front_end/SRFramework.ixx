module;
#include <SDL2/SDL.h>
#include <memory>
export module CPURender;
import apiAbstractor;
import SDLWindow;
namespace RGL {
	namespace swr {
		export class SoftwareRenderer : public RendererContext {

			void* framebuffer;

			std::shared_ptr<SDLWindow> sdlWindow;
			SDL_Surface* surface;

		public:
			void render() override {
				(*renderer)();
			}
			SoftwareRenderer(std::shared_ptr<SDLWindow> window):sdlWindow(window){
				
				
				surface = SDL_GetWindowSurface(*sdlWindow);
				SDL_FillRect(surface, 0, 0);
				SDL_UpdateWindowSurface(*sdlWindow);
				
			}
		};


	}
}
