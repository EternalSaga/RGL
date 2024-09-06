#pragma once
#include <glad/glad.h>
#include <glm/detail/qualifier.hpp>
#include <glm/fwd.hpp>
#include <type_traits>
#include <utility>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <tuple>
#include "Helpers.hpp"
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
					glCall(glUniform1f,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLint, Args...>)
				{

					glCall(glUniform1i,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLuint, Args...>)
				{
					glCall(glUniform1ui,location, std::forward<Args>(values)...);
				}
				else
				{
					static_assert(false, "Error template type");
				}
			}
			else if constexpr (numOfArgs == 2) {

				if constexpr (all_same_type_v<float, Args...>) {
					glCall(glUniform2f,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLint, Args...>)
				{
					glCall(glUniform2i,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLuint, Args...>)
				{
					glCall(glUniform2ui,location, std::forward<Args>(values)...);
				}
				else
				{
					static_assert(false, "Error template type");
				}
			}
			else if constexpr (numOfArgs == 3) {
				if constexpr (all_same_type_v<float, Args...>) {
					glCall(glUniform3f,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLint, Args...>)
				{
					glCall(glUniform3i,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLuint, Args...>)
				{
					glCall(glUniform3ui,location, std::forward<Args>(values)...);
				}
				else
				{
					static_assert(false, "Error template type");
				}
			}
			else if constexpr (numOfArgs == 4)
			{
				if constexpr (all_same_type_v<float, Args...>) {
					glCall(glUniform4f,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLint, Args...>)
				{
					glCall(glUniform4i,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLuint, Args...>)
				{
					glCall(glUniform4ui,location, std::forward<Args>(values)...);
				}
				else
				{
					static_assert(false, "Error template type");
				}
			}
		}
		



		template<glm::length_t vecSize,typename T, glm::qualifier q>
		void glUniform(GLint location,glm::vec<vecSize,T,q> vec){
			if constexpr (vecSize == 1) {
			    glUniform(location, vec.x);
			}
			else if constexpr (vecSize == 2) {
				glUniform(location, vec.x, vec.y);
			}
			else if constexpr (vecSize == 3) {
				glUniform(location, vec.x, vec.y, vec.z);
			}
			else if constexpr (vecSize == 4) {
				glUniform(location, vec.x, vec.y, vec.z, vec.w);
			}
			else {
				static_assert(false, "Error template type");
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
				else if constexpr (std::is_same_v<T, GLint>)
				{
					glUniform1iv(location, uniformVarCount, data);
				}
				else if constexpr (std::is_same_v<T, GLuint>) {
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
				else if constexpr (std::is_same_v<T, GLuint>) {
					glUniform2iuv(location, uniformVarCount, data);
				}
			}
			else if constexpr (UVarLength == 3) {
				if constexpr (std::is_same_v<T, float>)
				{
					glUniform3fv(location, uniformVarCount, data);
				}
				else if constexpr (std::is_same_v<T, GLint>)
				{
					glUniform3iv(location, uniformVarCount, data);
				}
				else if constexpr (std::is_same_v<T, GLuint>) {
					glUniform3iuv(location, uniformVarCount, data);
				}
			}
			else if constexpr (UVarLength == 4) {
				if constexpr (std::is_same_v<T, float>)
				{
					glUniform4fv(location, uniformVarCount, data);
				}
				else if constexpr (std::is_same_v<T, GLint>)
				{
					glUniform4iv(location, uniformVarCount, data);

				}
				else if constexpr (std::is_same_v<T, GLuint>) {
					glUniform4iuv(location, uniformVarCount, data);
				}
			}
		}
		


		template<int Cols, int Rows,glm::qualifier q>
		void glUniformMatrix(GLint location, const glm::mat<Cols, Rows,glm::f32, q>& value) {

				if constexpr (Cols == 2)
				{
					if constexpr (Rows == 2) {
						glCall(glUniformMatrix2fv,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else if constexpr (Rows == 3) {
						glCall(glUniformMatrix2x3fv,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else if constexpr (Rows == 4) {
						glCall(glUniformMatrix2x4fv,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else
					{
						static_assert(false, "Unsupport matrix type");
					}
				}
				else if constexpr (Cols == 3) {
					if constexpr (Rows == 2) {
						glCall(glUniformMatrix3x2fv,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else if constexpr (Rows == 3) {
						glCall(glUniformMatrix3fv,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else if constexpr (Rows == 4) {
						glCall(glUniformMatrix3x4fv,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else
					{
						static_assert(false, "Unsupport matrix type");
					}
				}
				else if constexpr (Cols == 4) {
					if constexpr (Rows == 2) {
						glCall(glUniformMatrix4x2fv,location, 1, GL_FALSE, glm::value_ptr(value));

					}
					else if constexpr (Rows == 3) {
						glCall(glUniformMatrix4x3fv,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else if constexpr (Rows == 4) {

						glCall(glUniformMatrix4fv,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else
					{
						static_assert(false, "Unsupport matrix type");
					}
				}


		}

	}
}
