module;
#include <SDL2/SDL.h>
export module softwareRenderer;
import apiAbstractor;
import RLApp;
namespace RGL {
	namespace swr {
		export class SoftwareRenderer : public RendererContext {

			void* framebuffer;
			int width;
			int height;
			SDL_Window* window;
		public:
			void render() override {

			}
			SoftwareRenderer() {
				//auto app = RGL::RLApp::getInstance();
				//window = app->getWindowHandler();
				//glfwGetFramebufferSize(window,&width,&height);
				
			}


		};
	}
}
