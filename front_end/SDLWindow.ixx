module;
#include <glad/glad.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <string>
#include <stdexcept>
export module SDLWindow;
import GLCheckError;
import apiAbstractor;
namespace RGL {
	export class SDLWindow {
	private:
		API_TYPE api_type;
		SDL_Window* window_;
		int width;
		int height;
	public:
		SDLWindow(int width, int height, std::string title, API_TYPE api_type) {
			this->width = width;
			this->height = height;
			if (0 != SDL_Init(SDL_INIT_EVERYTHING)) {
				throw std::runtime_error("SDL init failed");
			}

			this->api_type = api_type;
			switch (this->api_type)
			{
			case RGL::API_TYPE::OPENGL46:

				window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

				break;
			case RGL::API_TYPE::CPU:
				window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, SDL_WINDOW_SHOWN);

				break;
			case RGL::API_TYPE::VULKAN13:
				break;
			default:
				break;
			}
			if (window_ == nullptr)
			{
				throw std::runtime_error("Init windows failed");
			}
			const auto logger = glcore::Logger::getInstance();
			logger->info("Create windows success");
		}

		~SDLWindow() {
			SDL_DestroyWindow(window_);
			SDL_Quit();
		}
		operator SDL_Window* () {
			return window_;
		}
		int getWidth() const { return this->width; }
		int getHeight() const { return this->height; }
	};
}

