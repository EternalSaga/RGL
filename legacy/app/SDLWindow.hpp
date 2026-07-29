#pragma once
#include <SDL3/SDL.h>
#include "api_types.hpp"
#include <string>
namespace RGL
{
class SDLWindow
{
  private:
    API_TYPE api_type;
    SDL_Window *window_;
    int width;
    int height;

  public:
    SDLWindow(int width, int height, std::string title,
	API_TYPE api_type);

    ~SDLWindow();
    inline operator SDL_Window *() { return window_; }
    inline int getWidth() const { return this->width; }
    inline int getHeight() const { return this->height; }
};
} // namespace RGL
