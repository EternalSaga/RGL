#pragma once
#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
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
 bool show_demo_window = true;
  ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
 ImGuiIO io;
};
} // namespace glcore
} // namespace RGL
