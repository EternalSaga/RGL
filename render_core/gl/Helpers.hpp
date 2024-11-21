#pragma once
#include <concepts>
#include <utility>
#include <functional>

#include "GLCheckError.hpp"
namespace RGL {
namespace glcore {

template <typename FuncType, typename... Args>
auto glCall(FuncType&& func, Args&&... args) -> decltype(std::invoke(func, std::forward<Args>(args)...)) {
    using RetType = decltype(std::invoke(func, std::forward<Args>(args)...));
    if constexpr (std::is_same_v<RetType, void>) {
	std::invoke(func, std::forward<Args>(args)...);
#ifndef NDEBUG
	glCheckError();
#endif	// !NDEBUG
    } else {
	const auto res = std::invoke(func, std::forward<Args>(args)...);
	glCheckError();
	return res;
    }
}

template <typename FuncType, typename... Args>
decltype(auto) glCallRet(FuncType&& func, Args&&... args) {
    decltype(func(std::forward<Args>(args)...)) res = func(std::forward<Args>(args)...);
#ifndef NDEBUG
    glCheckError();
#endif	// !NDEBUG
    return res;
}

}  // namespace glcore

}  // namespace RGL
