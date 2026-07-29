#include "SDLWindow.hpp"
#include <SDL3/SDL_properties.h>
#include <stdexcept>
namespace RGL
{

SDLWindow::SDLWindow(int width, int height, std::string title,
    API_TYPE api_type) : window_(nullptr)
{
    this->width = width;
    this->height = height;
    if (!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS )) {
	throw std::runtime_error("SDL init failed");
    }

    this->api_type = api_type;
    switch (this->api_type) {
    case RGL::API_TYPE::OPENGL46:{
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title.c_str());
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, 800);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, 800);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        window_ = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
        break;
    }

	
    case RGL::API_TYPE::CPU:{
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title.c_str());
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, 800);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, 800);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, SDL_WINDOW_RESIZABLE);
        window_ = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
        break;
        
    }

    case RGL::API_TYPE::VULKAN13:
	break;
    default:
	break;
    }
    if (window_ == nullptr) {
	throw std::runtime_error("Init windows failed");
    }
}
SDLWindow::~SDLWindow()
{
    SDL_DestroyWindow(window_);
    SDL_Quit();
}
} // namespace RGL
