#pragma once
#include <entt/entt.hpp>

#include <mutex>
namespace RGL {

	using reactive_storage = entt::reactive_mixin<entt::storage<void>>;
	class EnttReg {
		static std::once_flag initOnce;
		static std::once_flag initStorageOnce;
	    static entt::registry* enttRegistry;
		static reactive_storage* storage;
		EnttReg() = default;
	public:
		static entt::registry* getPrimaryRegistry();
	 static reactive_storage* getPrimaryStorage();
	 ~EnttReg();
		
	};
	
	class SingleReg {
	   protected:
	    entt::registry* singleReg;
	    reactive_storage* singleStorage;
	   public:
	    SingleReg();
	};


	}
