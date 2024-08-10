module;
#include <vector>
#include <memory>
#include <glad/glad.h>
#include "Helpers.hpp"
#include <GLFW/glfw3.h>
#include "VertexDescriptor.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp> 
export module ThreeDEffectPractice;
import GLObjWrapper;

import GLFramework;

import Shader;
import GLTexture;
namespace RGL {
	namespace practice {
		using namespace glcore;
		const std::vector<float> rectangle_pos_uv2 = {
			-0.5f, -0.5f, 0.0f,0.0f, 0.0f,
			0.5f, -0.5f, 0.0f,1.0f, 0.0f,
			-0.5f,  0.5f, 0.0f,0.0f, 1.0f,
			0.5f,  0.5f, 0.0f,1.0f, 1.0f,
		};
		const std::vector<GLint> rectangle_indeces = {
			0, 1, 2,
			2, 1, 3
		};
		export class BasicTransform : public GLRenderer {
			std::unique_ptr<VBO> vbo;
			std::unique_ptr<VAO> vao;
			std::unique_ptr<Shader> shader;
			std::unique_ptr<EBO> ebo;
			std::unique_ptr<Texture> grass_land_noise;
			glm::mat4 trans;

			void doTransform() {
				trans = glm::rotate(trans, glm::radians(1.0f), glm::vec3(0, 0, 1));
			}

		public:
			BasicTransform():trans(1.0f) {

				

				vbo = std::make_unique<VBO>();
				vbo->setData(rectangle_pos_uv2);
				vao = std::make_unique<VAO>();
				ShaderSrcs shaders = {
					{SHADER_TYPE::VERTEX,{"shaders\\beginner\\BasicTransform.vert"}},
					{SHADER_TYPE::FRAGMENT,{"shaders\\beginner\\Blend.frag"}}
				};
				shader = std::make_unique<Shader>(shaders);
				grass_land_noise = std::make_unique<Texture>(3);
				{
					io::LoadedImg grassImg("./assest/grass.jpg");
					grass_land_noise->set(grassImg, 0, true);
				}
				{
					io::LoadedImg landImg("./assest/land.jpg");
					grass_land_noise->set(landImg, 1, true);
				}
				{
					io::LoadedImg noiseImg("./assest/noise.jpg");
					grass_land_noise->set(noiseImg, 2, true);
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

				shader->setUniform("grass", grass_land_noise->getTextureUnitID(0));
				shader->setUniform("mudLand", grass_land_noise->getTextureUnitID(1));
				shader->setUniform("randomNoise", grass_land_noise->getTextureUnitID(2));
				doTransform();
				shader->setUniformMat("transformMat", trans);//注意保证host和device端的类型一致，别把shader类型写成vec4

				glCall(glDrawElements, GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
			}
		};

	}
}
