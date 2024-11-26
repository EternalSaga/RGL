#pragma once
#include <glad/glad.h>
#include <glm/detail/qualifier.hpp>
#include <glm/fwd.hpp>
#include <type_traits>
#include <utility>


#include "Helpers.hpp"
namespace RGL {
	namespace glcore {

		

		template <typename T, typename... Args>
		constexpr bool all_same_type_v = (std::is_same_v<T, Args> && ...);

		template<typename... Args>
		void glUniform(GLuint program, GLint location, Args... values) {
			constexpr GLuint numOfArgs = sizeof...(values);
			static_assert(numOfArgs < 5, "Size of arguments must less than 5");
			if constexpr (numOfArgs == 1) {
				if constexpr (all_same_type_v<float, Args...>) {
					glCall(glProgramUniform1f,program,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLint, Args...>)
				{

					glCall(glProgramUniform1i,program,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLuint, Args...>)
				{
					glCall(glProgramUniform1ui,program,location, std::forward<Args>(values)...);
				}
				else
				{
					static_assert(false, "Error template type");
				}
			}
			else if constexpr (numOfArgs == 2) {

				if constexpr (all_same_type_v<float, Args...>) {
					glCall(glProgramUniform2f,program,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLint, Args...>)
				{
					glCall(glProgramUniform2i,program,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLuint, Args...>)
				{
					glCall(glProgramUniform2ui,program,location, std::forward<Args>(values)...);
				}
				else
				{
					static_assert(false, "Error template type");
				}
			}
			else if constexpr (numOfArgs == 3) {
				if constexpr (all_same_type_v<float, Args...>) {
					glCall(glProgramUniform3f,program,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLint, Args...>)
				{
					glCall(glProgramUniform3i,program,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLuint, Args...>)
				{
					glCall(glProgramUniform3ui,program,location, std::forward<Args>(values)...);
				}
				else
				{
					static_assert(false, "Error template type");
				}
			}
			else if constexpr (numOfArgs == 4)
			{
				if constexpr (all_same_type_v<float, Args...>) {
					glCall(glProgramUniform4f,program,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLint, Args...>)
				{
					glCall(glProgramUniform4i,program,location, std::forward<Args>(values)...);
				}
				else if constexpr (all_same_type_v<GLuint, Args...>)
				{
					glCall(glProgramUniform4ui,program,location, std::forward<Args>(values)...);
				}
				else
				{
					static_assert(false, "Error template type");
				}
			}
		}
		



		template<glm::length_t vecSize,typename T, glm::qualifier q>
		void glUniform(GLuint program, GLint location,glm::vec<vecSize,T,q> vec){
			if constexpr (vecSize == 1) {
			    glUniform(program,location,vec.x);
			}
			else if constexpr (vecSize == 2) {
				glUniform(program,location,vec.x, vec.y);
			}
			else if constexpr (vecSize == 3) {
				glUniform(program,location,vec.x, vec.y, vec.z);
			}
			else if constexpr (vecSize == 4) {
				glUniform(program,location,vec.x, vec.y, vec.z, vec.w);
			}
			else {
				static_assert(false, "Error template type");
			}
		}


		template<GLuint UVarLength, typename T>
		void glUniformVec(GLuint program, GLint location, GLsizei uniformVarCount, T* data) {
			assert(uniformVarCount < 5);
			if constexpr (UVarLength == 1) {
				if constexpr (std::is_same_v<T, float>)
				{
					glProgramUniform1fv(program,location,uniformVarCount, data);
				}
				else if constexpr (std::is_same_v<T, GLint>)
				{
					glProgramUniform1iv(program,location,uniformVarCount, data);
				}
				else if constexpr (std::is_same_v<T, GLuint>) {
					glProgramUniform1uiv(program,location,uniformVarCount, data);
				}
			}
			else if constexpr (UVarLength == 2)
			{
				if constexpr (std::is_same_v<T, float>)
				{
					glProgramUniform2fv(program,location,uniformVarCount, data);
				}
				else if constexpr(std::is_same_v<T, GLint>)
				{
					glProgramUniform2iv(program,location,uniformVarCount, data);

				}
				else if constexpr (std::is_same_v<T, GLuint>) {
					glProgramUniform2iuv(program,location,uniformVarCount, data);
				}
			}
			else if constexpr (UVarLength == 3) {
				if constexpr (std::is_same_v<T, float>)
				{
					glProgramUniform3fv(program,location,uniformVarCount, data);
				}
				else if constexpr (std::is_same_v<T, GLint>)
				{
					glProgramUniform3iv(program,location,uniformVarCount, data);
				}
				else if constexpr (std::is_same_v<T, GLuint>) {
					glProgramUniform3iuv(program,location,uniformVarCount, data);
				}
			}
			else if constexpr (UVarLength == 4) {
				if constexpr (std::is_same_v<T, float>)
				{
					glProgramUniform4fv(program,location,uniformVarCount, data);
				}
				else if constexpr (std::is_same_v<T, GLint>)
				{
					glProgramUniform4iv(program,location,uniformVarCount, data);

				}
				else if constexpr (std::is_same_v<T, GLuint>) {
					glProgramUniform4iuv(program,location,uniformVarCount, data);
				}
			}
		}
		


		template<int Cols, int Rows,glm::qualifier q>
		void glUniformMatrix(GLuint program, GLint location, const glm::mat<Cols, Rows,glm::f32, q>& value) {

				if constexpr (Cols == 2)
				{
					if constexpr (Rows == 2) {
						glCall(glProgramUniformMatrix2fv,program,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else if constexpr (Rows == 3) {
						glCall(glProgramUniformMatrix2x3fv,program,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else if constexpr (Rows == 4) {
						glCall(glProgramUniformMatrix2x4fv,program,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else
					{
						static_assert(false, "Unsupport matrix type");
					}
				}
				else if constexpr (Cols == 3) {
					if constexpr (Rows == 2) {
						glCall(glProgramUniformMatrix3x2fv,program,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else if constexpr (Rows == 3) {
						glCall(glProgramUniformMatrix3fv,program,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else if constexpr (Rows == 4) {
						glCall(glProgramUniformMatrix3x4fv,program,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else
					{
						static_assert(false, "Unsupport matrix type");
					}
				}
				else if constexpr (Cols == 4) {
					if constexpr (Rows == 2) {
						glCall(glProgramUniformMatrix4x2fv,program,location, 1, GL_FALSE, glm::value_ptr(value));

					}
					else if constexpr (Rows == 3) {
						glCall(glProgramUniformMatrix4x3fv,program,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else if constexpr (Rows == 4) {

						glCall(glProgramUniformMatrix4fv,program,location, 1, GL_FALSE, glm::value_ptr(value));
					}
					else
					{
						static_assert(false, "Unsupport matrix type");
					}
				}
		}


	}
}
