#pragma once
#include <entt/entt.hpp>
#include <mutex>
namespace RGL {
	class EnttReg {
		static std::once_flag initOnce;
	    static entt::registry* enttRegistry;
		EnttReg() = default;
	public:
		static entt::registry* getPrimaryRegistry();
	 ~EnttReg();
		
	};

	class SingleReg {
	   protected:
	    entt::registry* singleReg;

	   public:
	    SingleReg();
	};
	}
