#pragma once
#include <glad/glad.h>
#include <type_traits>
#include <utility>
namespace RGL {
	namespace glcore {
		template <typename T, typename... Args>
		constexpr bool all_same_type_v = (std::is_same_v<T, Args> && ...);

		template<typename... Args>
		void glUniform(GLint location, Args... values) {
			constexpr GLuint numOfArgs = sizeof...(values);
			static_assert(numOfArgs < 5, "Size of arguments must less than 5");
			if constexpr (numOfArgs == 1) {
				if constexpr (all_same_type_v<float, Args...>) {
					glUniform1f(location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLint, Args...>)
				{
					glUniform1i(location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLuint, Args...>)
				{
					glUniform1ui(location, std::forward<Args>(values)...);
				}
				else
				{
					static_assert(false, "Error template type");
				}
			}
			else if constexpr (numOfArgs == 2) {

				if constexpr (all_same_type_v<float, Args...>) {
					glUniform2f(location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLint, Args...>)
				{
					glUniform2i(location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLuint, Args...>)
				{
					glUniform2ui(location, std::forward<Args>(values)...);
				}
				else
				{
					static_assert(false, "Error template type");
				}
			}
			else if constexpr (numOfArgs == 3) {
				if constexpr (all_same_type_v<float, Args...>) {
					glUniform3f(location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLint, Args...>)
				{
					glUniform3i(location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLuint, Args...>)
				{
					glUniform3ui(location, std::forward<Args>(values)...);
				}
				else
				{
					static_assert(false, "Error template type");
				}
			}
			else if constexpr (numOfArgs == 4)
			{
				if constexpr (all_same_type_v<float, Args...>) {
					glUniform4f(location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLint, Args...>)
				{
					glUniform4i(location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLuint, Args...>)
				{
					glUniform4ui(location, std::forward<Args>(values)...);
				}
				else
				{
					static_assert(false, "Error template type");
				}
			}
		}

		template<GLuint UVarLength, typename T>
		void glUniformVec(GLint location, GLsizei uniformVarCount, T* data) {
			assert(uniformVarCount < 5);
			if constexpr (UVarLength == 1) {
				if constexpr (std::is_same_v<T, float>)
				{
					glUniform1fv(location, uniformVarCount, data);
				}
				else if (std::is_same_v<T, GLint>)
				{
					glUniform1iv(location, uniformVarCount, data);
				}
				else if (std::is_same_v<T, GLuint>) {
					glUniform1uiv(location, uniformVarCount, data);
				}
			}
			else if constexpr (UVarLength == 2)
			{
				if constexpr (std::is_same_v<T, float>)
				{
					glUniform2fv(location, uniformVarCount, data);
				}
				else if (std::is_same_v<T, GLint>)
				{
					glUniform2iv(location, uniformVarCount, data);

				}
				else if (std::is_same_v<T, GLuint>) {
					glUniform2iuv(location, uniformVarCount, data);
				}
			}
			else if constexpr (UVarLength == 3) {
				if constexpr (std::is_same_v<T, float>)
				{
					glUniform3fv(location, uniformVarCount, data);
				}
				else if (std::is_same_v<T, GLint>)
				{
					glUniform3iv(location, uniformVarCount, data);
				}
				else if (std::is_same_v<T, GLuint>) {
					glUniform3iuv(location, uniformVarCount, data);
				}
			}
			else if constexpr (UVarLength == 4) {
				if constexpr (std::is_same_v<T, float>)
				{
					glUniform4fv(location, uniformVarCount, data);
				}
				else if (std::is_same_v<T, GLint>)
				{
					glUniform4iv(location, uniformVarCount, data);

				}
				else if (std::is_same_v<T, GLuint>) {
					glUniform4iuv(location, uniformVarCount, data);
				}
			}
		}

	}
}
