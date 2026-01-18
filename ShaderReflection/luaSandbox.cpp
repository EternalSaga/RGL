#include "luaSandbox.hpp"
#include <memory>
#include <mutex>
#include <sol/state.hpp>
#include <sol/types.hpp>
namespace RGL {
namespace reflection {

std::shared_ptr<sol::state> LuaSandbox::lua = nullptr;
std::once_flag LuaSandbox::initOnce;
LuaSandbox::LuaSandbox() {
    lua = std::make_shared<sol::state>();
    lua->open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
    (*lua)["dofile"] = sol::nil;
    (*lua)["loadfile"] = sol::nil;
    (*lua)["package"] = sol::nil;
    (*lua)["require"] = sol::nil;
}

std::shared_ptr<sol::state> LuaSandbox::getLuaState(){
    std::call_once(initOnce, [](){
        LuaSandbox();
    });
    return lua;
}

}  // namespace reflection
}  // namespace RGL