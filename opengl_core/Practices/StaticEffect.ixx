module;

#include <vector>
#include <memory>
#include <glad/glad.h>
#include "Helpers.hpp"
export module StaticEffectPractice;
import GLObjWrapper;

import GLFramework;

import Shader;
namespace RGL {
	namespace glcore {
		using namespace io;
		const std::vector<float> positions{
			-0.5f,-0.5f,0.0f,
			0.5f,-0.5f,0.0f,
			0.5f,0.5f,0.0f
		};
		const std::vector<float> colors{
			1,0,0,
			0,1,0,
			0,0,1
		};
		const std::vector<float> pos_col_interleaved{
			-0.5f,-0.5f,0.0f,1.f,0.f,0.f,
			0.5f,-0.5f,0.0f,0.f,1.f,0.f,
			0.0f,0.5f,0.0f,0.f,0.f,1.f
		};

		const std::vector<float> positions4{
			-0.5f, -0.5f, 0.0f,
			0.5f, -0.5f, 0.0f,
			0.0f,  0.5f, 0.0f,
			0.5f,  0.5f, 0.0f,
		};

		const std::vector<GLint> eboIndices{
			0, 1, 2,
			2, 1, 3
		};




		export class EBOExec :public GLRenderer
		{
			std::unique_ptr<EBO> ebo;
			std::unique_ptr<VAO> vao;
			std::unique_ptr<VBO> vbo;
			std::unique_ptr<Shader> shader;
		public:
			EBOExec() {
				vbo = std::make_unique<VBO>();
				vbo->setData(positions4);
				ebo = std::make_unique<EBO>();
				ebo->setData(eboIndices);
				vao = std::make_unique<VAO>();


				ShaderSrcs shaders = {
				{SHADER_TYPE::VERTEX,{"shaders\\beginner\\shader.vert"}},
				{SHADER_TYPE::FRAGMENT,{"shaders\\beginner\\shader.frag"}}
				};

				shader = std::make_unique<Shader>(shaders);
				vao->setShaderProgram(*shader);
				vao->set(*vbo, 3, "aPos");
				vao->addEBO(*ebo);

			}
			virtual ~EBOExec() = default;
			void operator()() override {
				shader->useProgram();
				glCall(glBindVertexArray, *vao);
				//glCall(glDrawElements, GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

				glCall(glDrawElements,GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

				glCall(glBindVertexArray,0);
			}
		private:

		};

		const std::vector<GLint> eboSimple{
	0, 1, 2
		};
		export class ColorfulTriangle :public GLRenderer
		{
			std::unique_ptr<EBO> ebo;
			std::unique_ptr<VAO> vao;
			std::unique_ptr<VBO> vbo;
			std::unique_ptr<Shader> shader;
		public:
			ColorfulTriangle() {
				vbo = std::make_unique<VBO>();
				vbo->setData(pos_col_interleaved);
				ebo = std::make_unique<EBO>();
				ebo->setData(eboSimple);
				vao = std::make_unique<VAO>();

				vao->addEBO(*ebo);

				ShaderSrcs shaders = {
					{SHADER_TYPE::VERTEX,{"shaders\\beginner\\outColor.vert"}},
					{SHADER_TYPE::FRAGMENT,{"shaders\\beginner\\colorful.frag"}}
				};
				shader = std::make_unique<Shader>(shaders);

				vao->setShaderProgram(*shader);

				vao->set(*vbo, 3, 6, 0, "inPos");

				vao->set(*vbo, 3, 6, 3, "inColor");

			}
			virtual ~ColorfulTriangle() = default;
			void operator()() override {
				shader->useProgram();
				glCall(glBindVertexArray, *vao);

				glCall(glDrawElements, GL_TRIANGLES, 3, GL_UNSIGNED_INT, nullptr);

				glBindVertexArray(0);	
			}
		};


		//export class SingleBuffer :public GLRenderer {
		//	std::unique_ptr<VBO> positionVbo;
		//	std::unique_ptr<VBO> colorVbo;
		//	std::unique_ptr<VAO> vao;
		//public:
		//	SingleBuffer() :GLRenderer() {
		//		positionVbo = std::make_unique<VBO>();
		//		positionVbo->setData(positions);
		//		colorVbo = std::make_unique<VBO>();
		//		colorVbo->setData(colors);

		//		vao = std::make_unique<VAO>();

		//		vao->set(*positionVbo, 3, BUFF_ATTRIBUTION::VERT_POSITION);

		//		vao->set(*colorVbo, 3, BUFF_ATTRIBUTION::COLOR);
		//	}
		//	virtual ~SingleBuffer() = default;
		//	void operator()() override {

		//	}
		//};

		export class InterLeavedBuffer : public GLRenderer {
			std::unique_ptr<VBO> posColorVbo;
			std::unique_ptr<VAO> vao;
			std::unique_ptr<Shader> shader;
		public:
			InterLeavedBuffer() :GLRenderer() {
				posColorVbo = std::make_unique<VBO>();
				posColorVbo->setData(pos_col_interleaved);

				vao = std::make_unique<VAO>();

				

				ShaderSrcs shaders = {
					{SHADER_TYPE::VERTEX,{"shaders\\beginner\\shader.vert"}},
					{SHADER_TYPE::FRAGMENT,{"shaders\\beginner\\shader.frag"}}
				};

				shader = std::make_unique<Shader>(shaders);

				vao->setShaderProgram(*shader);

				vao->set(*posColorVbo, 3, 6, 0,"aPos");

				vao->set(*posColorVbo, 3, 6, 3, "inColor");
			}
			void operator()() override {
				shader->useProgram();
				glCall(glBindVertexArray, *vao);
				glCall(glDrawArrays, GL_TRIANGLES, 0, 3);
			}
			virtual ~InterLeavedBuffer() = default;
		};

	}
}
