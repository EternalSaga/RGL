#include "EnttRegistry.hpp"
#include <glm/glm.hpp>
namespace RGL {
struct MouseKeyboardControlComponent {
    bool leftDown;
    bool rightDown;
    bool middleDown;

    bool shouldQuit;

    glm::vec2 currentCursor;
    glm::vec2 leftdownCursor;
    glm::vec2 rightdownCursor;
    glm::vec2 lastCursor;
    std::map<int, bool> keyMap;
};

class TrackBallMouseKeyboardSystem {
    entt::registry *singleReg;
    TrackBallMouseKeyboardSystem();

   public:
    void update();
};
class TrackballSystem {
    entt::registry *singleReg;
    TrackballSystem();

   public:
    void update();
};
	}
