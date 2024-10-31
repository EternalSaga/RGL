#include "EnttRegistry.hpp"

namespace RGL {

	std::once_flag EnttReg::initOnce{};
entt::registry* EnttReg::enttRegistry{nullptr};

entt::registry* EnttReg::getPrimaryRegistry() {
    std::call_once(initOnce, []() {
	enttRegistry = new entt::registry();
    });
    return enttRegistry;
}

EnttReg::~EnttReg() {
    if (enttRegistry)
	delete enttRegistry;
    enttRegistry = nullptr;
}

}  // namespace RGL
