module;

#include <vector>
#include <memory>
#include <glad/glad.h>
#include "Helpers.hpp"
#include <GLFW/glfw3.h>
export module DynamicEffectPractice;
import GLObjWrapper;

import GLFramework;

import Shader;
namespace RGL {
	namespace glcore {
		using namespace io;

		const std::vector<float> pos_col_interleaved{
			-0.5f,-0.5f,0.0f,1.f,0.f,0.f,
			0.5f,-0.5f,0.0f,0.f,1.f,0.f,
			0.0f,0.5f,0.0f,0.f,0.f,1.f
		};
		const std::vector<GLint> eboSimple{
			0, 1, 2
		};

		export class ColorfulShiningTriangle :public GLRenderer
		{
			std::unique_ptr<EBO> ebo;
			std::unique_ptr<VAO> vao;
			std::unique_ptr<VBO> vbo;
			std::unique_ptr<Shader> shader;
		public:
			ColorfulShiningTriangle() {
				vbo = std::make_unique<VBO>();
				vbo->setData(pos_col_interleaved);
				ebo = std::make_unique<EBO>();
				ebo->setData(eboSimple);
				vao = std::make_unique<VAO>();

				vao->addEBO(*ebo);

				ShaderSrcs shaders = {
					{SHADER_TYPE::VERTEX,{"shaders\\beginner\\UniformColor.vert"}},
					{SHADER_TYPE::FRAGMENT,{"shaders\\beginner\\UniformColor.frag"}}
				};
				shader = std::make_unique<Shader>(shaders);

				vao->setShaderProgram(*shader);

				vao->set(*vbo, 3, 6, 0, "inPos");

				vao->set(*vbo, 3, 6, 3, "inColor");

			}
			virtual ~ColorfulShiningTriangle() = default;
			void operator()() override {
				shader->useProgram();
				shader->setUniformFloat("time", glfwGetTime());

				glCall(glBindVertexArray, *vao);

				glCall(glDrawElements, GL_TRIANGLES, 3, GL_UNSIGNED_INT, nullptr);

				glBindVertexArray(0);
			}
		};
	}
}
