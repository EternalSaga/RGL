#include "ControlLogic.hpp"
#include <imgui.h>
#include <imgui_impl_sdl2.h>

namespace RGL
{

bool
ControlLogic::dealWithEvent()
{
    SDL_Event sdlevent;
    bool quit = false;
    while (SDL_PollEvent(&sdlevent)) {
	ImGui_ImplSDL2_ProcessEvent(&sdlevent);
	if (sdlevent.type == SDL_QUIT) {
	    quit = true;
	} else if (sdlevent.type == SDL_WINDOWEVENT) {
	    if (sdlevent.window.event == SDL_WINDOWEVENT_RESIZED) {
		onWindowResizeCbk(sdlevent.window);
	    }
	} else if (sdlevent.type == SDL_KEYDOWN) {
	    onKeyboardDownCbk(sdlevent.key);
	} else if (sdlevent.type == SDL_KEYUP) {
	    onKeyboardUpCbk(sdlevent.key);
	} else if (sdlevent.type == SDL_MOUSEBUTTONDOWN) {
	    onMouseDownCbk(sdlevent.button);
	} else if (sdlevent.type == SDL_MOUSEBUTTONUP) {
	    onMouseUpCbk(sdlevent.button);
	} else if (sdlevent.type == SDL_MOUSEWHEEL) {
	    onMouseWheelCbk(sdlevent.wheel);
	} else if (sdlevent.type == SDL_MOUSEMOTION) {
	    onCursorMoveCbk(sdlevent.motion);
	}
    }
    return quit;
}

void
TestLogControlLogic::onKeyboardDownCbk(
    const SDL_KeyboardEvent &keyboardEvt)
{
    logger->info("Keyboard Down: {}", keyboardEvt.keysym.sym);
}
void
TestLogControlLogic::onKeyboardUpCbk(
    const SDL_KeyboardEvent &keyboardEvt)
{
    logger->info("Keyboard Up: {}", keyboardEvt.keysym.sym);
}
void
TestLogControlLogic::onMouseDownCbk(const SDL_MouseButtonEvent &mouseEvt)
{
    logger->info("Mouse Down: {}", mouseEvt.button);
}
void
TestLogControlLogic::onMouseUpCbk(const SDL_MouseButtonEvent &mouseEvt)
{
    logger->info("Mouse Up: {}", mouseEvt.button);
}
void
TestLogControlLogic::onMouseMoveCbk(const SDL_MouseMotionEvent &mouseEvt)
{
    logger->info("Mouse Move: {} {}", mouseEvt.x, mouseEvt.y);
}
void
TestLogControlLogic::onMouseWheelCbk(const SDL_MouseWheelEvent &wheelEvt)
{
    logger->info("Mouse Wheel: {}", wheelEvt.y);
}
void
TestLogControlLogic::onWindowResizeCbk(const SDL_WindowEvent &windowEvt)
{
    logger->info("Window Resize: {} {}", windowEvt.data1, windowEvt.data2);
}
void
TestLogControlLogic::onCursorMoveCbk(
    const SDL_MouseMotionEvent &cursorEvt)
{
    logger->info("Cursor Move: {} {}", cursorEvt.x, cursorEvt.y);
}
TestLogControlLogic::TestLogControlLogic()
{
}
void
ControlLogic::setCamera(std::shared_ptr<Camera> camera)
{
    this->camera = camera;
}
} // namespace RGL
