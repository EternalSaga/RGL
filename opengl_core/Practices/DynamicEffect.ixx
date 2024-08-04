module;
#include <vector>
#include <memory>
#include <glad/glad.h>
#include "Helpers.hpp"
#include <GLFW/glfw3.h>
#include "VertexDescriptor.hpp"
export module DynamicEffectPractice;
import GLObjWrapper;

import GLFramework;

import Shader;
import GLTexture;
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
				shader->setUniform<float>("time", glfwGetTime());

				glCall(glBindVertexArray, *vao);
				glCall(glDrawElements, GL_TRIANGLES, 3, GL_UNSIGNED_INT, nullptr);
				glCall(glBindVertexArray, 0);

			}
		};

		export class MovingTriangle :public GLRenderer {
			std::unique_ptr<EBO> ebo;
			std::unique_ptr<VAO> vao;
			std::unique_ptr<VBO> vbo;
			std::unique_ptr<Shader> shader;
		public:
			MovingTriangle() {
				vbo = std::make_unique<VBO>();
				vbo->setData(pos_col_interleaved);
				ebo = std::make_unique<EBO>();
				ebo->setData(eboSimple);
				vao = std::make_unique<VAO>();

				vao->addEBO(*ebo);

				ShaderSrcs shaders = {
					{SHADER_TYPE::VERTEX,{"shaders\\beginner\\moving.vert"}},
					{SHADER_TYPE::FRAGMENT,{"shaders\\beginner\\moving.frag"}}
				};
				shader = std::make_unique<Shader>(shaders);

				vao->setShaderProgram(*shader);
				vao->set(*vbo, 3, 6, 0, "inPos");
				vao->set(*vbo, 3, 6, 3, "inColor");
			}
			virtual ~MovingTriangle() = default;
			void operator()() override {
				shader->useProgram();
				shader->setUniform<float>("time", glfwGetTime());

				glCall(glBindVertexArray, *vao);
				glCall(glDrawElements, GL_TRIANGLES, 3, GL_UNSIGNED_INT, nullptr);
			}
		};

		const std::vector<float> rectangle_pos_uv = {
			-0.5f, -0.5f, 0.0f,0.0f, 0.0f,
			0.5f, -0.5f, 0.0f,1.3f, 0.0f,
			-0.5f,  0.5f, 0.0f,0.0f, 1.0f,
			0.5f,  0.5f, 0.0f,1.3f, 1.0f,
		};
		const std::vector<GLint> rectangle_indeces = {
			0, 1, 2,
			2, 1, 3
		};



		export class MovingTexture : public GLRenderer {
			std::unique_ptr<VBO> vbo;
			std::unique_ptr<VAO> vao;
			std::unique_ptr<Shader> shader;
			std::unique_ptr<EBO> ebo;
			std::unique_ptr<Texture> texture;
		public:
			MovingTexture() {
				vbo = std::make_unique<VBO>();
				vbo->setData(rectangle_pos_uv);

				vao = std::make_unique<VAO>();
				ShaderSrcs shaders = {
					{SHADER_TYPE::VERTEX,{"shaders\\beginner\\movinguv.vert"}},
					{SHADER_TYPE::FRAGMENT,{"shaders\\beginner\\movinguv.frag"}}
				};
				shader = std::make_unique<Shader>(shaders);

				texture = std::make_unique<Texture>();
				{
					io::LoadedImg img("./assest/001.jpg");
					texture->set(img,0);
				}

				vao->setShaderProgram(*shader);


				auto desc = hana::make_tuple(VertexElement<float[3]>("inPos"), VertexElement<float[2]>("inUV"));

				vao->setDSA_interleaved(*vbo, desc);

				ebo = std::make_unique<EBO>();

				ebo->setData(rectangle_indeces);

				vao->addEBO(*ebo);
			}
			void operator()() override {
				shader->useProgram();

				shader->setUniform("sampler", texture->getTextureUnitID());
				shader->setUniform<float>("timestamp", glfwGetTime());
				glCall(glBindVertexArray, *vao);
				glCall(glDrawElements, GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
			}
		};

		struct Vetex_UV
		{
			GLfloat position[3];
			GLfloat uv[2];
		};

	}
}
