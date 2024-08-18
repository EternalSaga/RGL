#pragma once

#include "SDLSurface.hpp"
#include "SDLWindow.hpp"
#include "api_types.hpp"
#include <SDL2/SDL.h>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <stdexcept>
#include <vector>

namespace RGL {
namespace swr {

class SoftwareRenderContext : public RendererContext {

  void *framebuffer;

  std::shared_ptr<SDLWindow> sdlWindow;

  SDL_Renderer *sdlRenderer;
  int width;
  int height;
  ColorAttachment colorSurface;
  void drawpixel();

public:
  void render() override;
  void SwapBuffer(SDL_Surface *surface);

  SoftwareRenderContext(std::shared_ptr<SDLWindow> window);

  ~SoftwareRenderContext();
};

} // namespace swr
} // namespace RGL
