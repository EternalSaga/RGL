#pragma once
#include <entt/entt.hpp>
#include <memory>
#include <mutex>
namespace RGL {

	using reactive_storage = entt::reactive_mixin<entt::storage<void>>;
	class EnttReg {
		static std::once_flag initOnce;
		static std::once_flag initStorageOnce;
		static std::once_flag initDispatcherOnce;
	    static std::unique_ptr<entt::registry> enttRegistry;
		static std::unique_ptr<reactive_storage> storage;
	    static std::unique_ptr<entt::dispatcher> dispatcher;
		EnttReg() = default;
	public:
		static entt::registry* getPrimaryRegistry();
		static reactive_storage* getPrimaryStorage();
		static entt::dispatcher& getDispatcher();
	 ~EnttReg();
		
	};
	
	class SingleReg {
	   protected:
	    entt::registry* singleReg;
	    reactive_storage* singleStorage;
	    entt::dispatcher& singleDispatcher;
	   public:
	    SingleReg();
	};


	}
