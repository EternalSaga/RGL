#pragma once
#include <sol/sol.hpp>
#include <mutex>
#include <sol/state.hpp>
#include <memory>
#include <stdexcept>
namespace RGL {
namespace reflection {
    class LuaSandbox{
        LuaSandbox();
        static std::shared_ptr<sol::state> lua;
        static std::once_flag initOnce;
        public:
        static std::shared_ptr<sol::state> getLuaState();

    };

    class LuaTableExcept : public std::runtime_error{
        public:
	 explicit LuaTableExcept(const std::string& msg);
    };
}
}