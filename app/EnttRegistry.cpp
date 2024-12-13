#include "EnttRegistry.hpp"

namespace RGL {

	std::once_flag EnttReg::initOnce{};
	std::once_flag EnttReg::initStorageOnce;
entt::registry* EnttReg::enttRegistry{nullptr};
	reactive_storage* EnttReg::storage{nullptr};
	entt::registry* EnttReg::getPrimaryRegistry() {
    std::call_once(initOnce, []() {
	enttRegistry = new entt::registry();
		
    });
    return enttRegistry;
	}

	reactive_storage* EnttReg::getPrimaryStorage() {
	    std::call_once(initStorageOnce, []() {
		if (!enttRegistry) {
		    throw std::logic_error("registry has not been initialized.");
		}

		storage = new reactive_storage();
		storage->bind(*enttRegistry);
			});
	    return storage;
	}



EnttReg::~EnttReg() {
    if (enttRegistry)
	delete enttRegistry;
    if (storage) {
	delete storage;
    }
    enttRegistry = nullptr;
	storage = nullptr;
}
SingleReg::SingleReg() {
    singleReg = EnttReg::getPrimaryRegistry();
    singleStorage = EnttReg::getPrimaryStorage();
}
}  // namespace RGL
