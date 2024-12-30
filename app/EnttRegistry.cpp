#include "EnttRegistry.hpp"

namespace RGL {

	std::once_flag EnttReg::initOnce{};
	std::once_flag EnttReg::initStorageOnce;
	std::once_flag EnttReg::initDispatcherOnce;
	std::unique_ptr <entt::registry> EnttReg::enttRegistry{nullptr};
	std::unique_ptr <reactive_storage> EnttReg::storage{nullptr};
	std::unique_ptr<entt::dispatcher> EnttReg::dispatcher{nullptr};
	entt::registry* EnttReg::getPrimaryRegistry() {
    std::call_once(initOnce, []() {
	enttRegistry.reset(new entt::registry());
		
    });
    return enttRegistry.get();
	}

	reactive_storage* EnttReg::getPrimaryStorage() {
	    std::call_once(initStorageOnce, []() {
		if (!enttRegistry) {
		    throw std::logic_error("registry has not been initialized.");
		}

		storage.reset(new reactive_storage());
		storage->bind(*enttRegistry);
			});
	    return storage.get();
	}

	entt::dispatcher& EnttReg::getDispatcher() {
	    std::call_once(initDispatcherOnce, []() {
		dispatcher.reset(new entt::dispatcher());
			});
	    return *dispatcher;
	}



EnttReg::~EnttReg() {


}
SingleReg::SingleReg(): singleDispatcher(EnttReg::getDispatcher()) {
    singleReg = EnttReg::getPrimaryRegistry();
    singleStorage = EnttReg::getPrimaryStorage();
}
}  // namespace RGL
