#pragma once
#include <glad/glad.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdexcept>
#include "Helpers.hpp"
#include <memory>
#include <cassert>
#include "api_types.hpp"
#include <imgui.h>
namespace RGL {
namespace glcore {
class GLContext : public RendererContext {
  SDL_GLContext glContext;

public:
  GLContext(SDL_Window *windowHandler, GLint viewPortWidth,
            GLint viewPortHeight);

  ~GLContext();

  void render() override;

private:
  SDL_Window *window_;
};
} // namespace glcore
} // namespace RGL
