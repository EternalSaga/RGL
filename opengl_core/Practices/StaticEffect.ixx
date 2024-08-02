module;

#include <vector>
#include <memory>
#include <glad/glad.h>
#include "Helpers.hpp"

export module StaticEffectPractice;
import GLObjWrapper;

import GLFramework;

import Shader;
import GLTexture;
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
			-0.5f, -0.5f, 0.0f,1,0,0,
		0.5f, -0.5f, 0.0f,0,1,0,
		0.0f,  0.5f, 0.0f,0,0,1
		};

		const std::vector<float> pos_uv_interleaved{
		-0.5f,-0.5f,0.0f,0.0f, 0.0f,
		0.5f,-0.5f,0.0f,1.0f, 0.0f,
		0.0f,0.5f,0.0f,0.5f, 1.0f,
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

				glCall(glDrawElements, GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

				glCall(glBindVertexArray, 0);
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

				vao->set(*posColorVbo, 3, 6, 0, "aPos");

				vao->set(*posColorVbo, 3, 6, 3, "inColor");
			}
			void operator()() override {
				shader->useProgram();
				glCall(glBindVertexArray, *vao);
				glCall(glDrawArrays, GL_TRIANGLES, 0, 3);
			}
			virtual ~InterLeavedBuffer() = default;
		};

		export class SetColorByUniform : public GLRenderer {
			std::unique_ptr<VBO> vbo;
			std::unique_ptr<VAO> vao;
			std::unique_ptr<Shader> shader;
		public:
			SetColorByUniform() {
				vbo = std::make_unique<VBO>();
				vbo->setData(positions);

				vao = std::make_unique<VAO>();
				ShaderSrcs shaders = {
					{SHADER_TYPE::VERTEX,{"shaders\\beginner\\UniformTriangle.vert"}},
					{SHADER_TYPE::FRAGMENT,{"shaders\\beginner\\UniformTriangle.frag"}}
				};
				shader = std::make_unique<Shader>(shaders);
				vao->setShaderProgram(*shader);

				vao->set(*vbo, 3, 3, 0, "inPos");

			}

			void operator()() override {
				shader->useProgram();
				shader->setUniform("ucolor", 1.0f, 1.f, 0.f);
				glCall(glBindVertexArray, *vao);
				glCall(glDrawArrays, GL_TRIANGLES, 0, 3);
			}
		};

		export class TexturePractice : public GLRenderer {
			std::unique_ptr<VBO> vbo;
			std::unique_ptr<VAO> vao;
			std::unique_ptr<Shader> shader;
			std::unique_ptr<EBO> ebo;
			std::unique_ptr<Texture> texture;

		public:
			TexturePractice() {
				vbo = std::make_unique<VBO>();
				vbo->setData(pos_uv_interleaved);

				vao = std::make_unique<VAO>();
				ShaderSrcs shaders = {
					{SHADER_TYPE::VERTEX,{"shaders\\beginner\\uvshader.vert"}},
					{SHADER_TYPE::FRAGMENT,{"shaders\\beginner\\uvshader.frag"}}
				};
				shader = std::make_unique<Shader>(shaders);

				texture = std::make_unique<Texture>();
				{
					io::LoadedImg img("./assest/001.jpg");
					texture->set(img,0);
				}

				vao->setShaderProgram(*shader);

				vao->set(*vbo, 3, 5, 0, "inPos");
				vao->set(*vbo, 2, 5, 3, "inUV");

				ebo = std::make_unique<EBO>();

				ebo->setData(eboSimple);

				vao->addEBO(*ebo);
			}

			void operator()() override {
				shader->useProgram();

				shader->setUniform("sampler", texture->getTextureUnitID());
				glCall(glBindVertexArray, *vao);
				glCall(glDrawElements, GL_TRIANGLES, 3, GL_UNSIGNED_INT, nullptr);
			}
			virtual ~TexturePractice() = default;
		};


		const std::vector<float> rectangle_pos_uv = {
			-0.5f, -0.5f, 0.0f,0.0f, 0.0f,
			0.5f, -0.5f, 0.0f,2.0f, 0.0f,
			-0.5f,  0.5f, 0.0f,0.0f, 2.0f,
			0.5f,  0.5f, 0.0f,2.0f, 2.0f,
		};
		const std::vector<GLint> rectangle_indeces ={
			0, 1, 2,
			2, 1, 3
		};

		export class RectangleTexture : public GLRenderer {
			std::unique_ptr<VBO> vbo;
			std::unique_ptr<VAO> vao;
			std::unique_ptr<Shader> shader;
			std::unique_ptr<EBO> ebo;
			std::unique_ptr<Texture> texture;
		public:
			RectangleTexture() {
				vbo = std::make_unique<VBO>();
				vbo->setData(rectangle_pos_uv);

				vao = std::make_unique<VAO>();
				ShaderSrcs shaders = {
					{SHADER_TYPE::VERTEX,{"shaders\\beginner\\uvshader.vert"}},
					{SHADER_TYPE::FRAGMENT,{"shaders\\beginner\\uvshader.frag"}}
				};
				shader = std::make_unique<Shader>(shaders);

				texture = std::make_unique<Texture>();
				{
					io::LoadedImg img("./assest/001.jpg");
					texture->set(img,0);
				}

				vao->setShaderProgram(*shader);

				vao->set(*vbo, 3, 5, 0, "inPos");
				vao->set(*vbo, 2, 5, 3, "inUV");

				ebo = std::make_unique<EBO>();

				ebo->setData(rectangle_indeces);

				vao->addEBO(*ebo);
			}
			void operator()() override {
				shader->useProgram();

				shader->setUniform("sampler", texture->getTextureUnitID());
				glCall(glBindVertexArray, *vao);
				glCall(glDrawElements, GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
			}
		};

	}
}
