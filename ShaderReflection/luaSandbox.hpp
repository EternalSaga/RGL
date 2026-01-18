#pragma once
#include <sol/sol.hpp>
#include <mutex>
#include <sol/state.hpp>
#include <memory>
namespace RGL {
namespace reflection {
    class LuaSandbox{
        LuaSandbox();
        static std::shared_ptr<sol::state> lua;
        static std::once_flag initOnce;
        public:
        static std::shared_ptr<sol::state> getLuaState();

    };

}
}