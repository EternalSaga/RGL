﻿#pragma once
#include <concepts>
#include <utility>
#include <functional>
import GLCheckError;

namespace RGL {
	namespace glcore {

		template <typename FuncType, typename... Args>
		void glCall(FuncType&& func, Args&&... args) {
			func(std::forward<Args>(args)...);
#ifndef NDEBUG
			glCheckError();		
#endif // !NDEBUG
		}

		template <typename FuncType, typename... Args>
		decltype(auto) glCallRet(FuncType&& func, Args&&... args) 			
		{
			decltype(func(std::forward<Args>(args)...)) res = func(std::forward<Args>(args)...);
#ifndef NDEBUG
			glCheckError();
#endif // !NDEBUG
			return res;
		}

	}

	template<typename FunctionSignitrue>
	decltype(auto) getFuncPtr(const std::function<FunctionSignitrue>& func)
	{
		return func.target<FunctionSignitrue>(func);
	}
}
