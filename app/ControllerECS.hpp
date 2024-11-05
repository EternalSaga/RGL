#pragma once
#include "EnttRegistry.hpp"
#include <glm/glm.hpp>
namespace RGL {
bool currentCursorBetween(const glm::vec2 &downCursor, const glm::vec2 &lastCursor, const glm::vec2 &currentCursor);

struct MouseKeyboardInput {

    bool leftDown;
    bool rightDown;
    bool middleDown;

    bool shouldQuit = false;

    glm::vec2 currentCursor;
    glm::vec2 leftdownCursor;
    glm::vec2 rightdownCursor;
    glm::vec2 lastCursor;
    std::map<int, bool> keyMap;
};

class TrackBallMouseKeyboardSystem : public SingleReg {
   public:
    void update();
};
class TrackballSystem : public SingleReg {
   public:
    void update();
};

class GameControlSystem : public SingleReg {
	public:
    void update();
};
bool ShouldQuit();
}  // namespace RGL
