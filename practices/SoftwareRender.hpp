
#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include <cstdint>
#include "SRFramework.hpp"
namespace RGL {
	namespace swr {
        void set_pixel(SDL_Surface *surface, int x, int y, Uint32 pixel);
        class TestCPURender : public Renderer {
          SDL_Surface *surface;
          int width;
          int height;
          SDL_Renderer *sdlRenderer;

        public:
          TestCPURender(SDL_Surface *surface);
          void operator()() override;
                };
	}
}
