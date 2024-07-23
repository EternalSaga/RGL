module;

#include <vector>
#include <memory>
#include <glad/glad.h>
#include "Helpers.hpp"
export module ExecsiceCodes;
import GLObjWrapper;

import GLFramework;

import LoadShader;
namespace RGL {
	namespace glcore {
		using namespace io;
		std::vector<float> positions{
			-0.5f,-0.5f,0.0f,
			0.5f,-0.5f,0.0f,
			0.5f,0.5f,0.0f
		};
		std::vector<float> colors{
			1,0,0,
			0,1,0,
			0,0,1
		};
		std::vector<float> pos_col_interleaved{
			-0.5f,-0.5f,0.0f,1.f,0.f,0.f,
			0.5f,-0.5f,0.0f,0.f,1.f,0.f,
			0.5f,0.5f,0.0f,0.f,0.f,1.f
		};



		export class SingleBuffer :public GLRenderer {
			std::unique_ptr<VBO> positionVbo;
			std::unique_ptr<VBO> colorVbo;
			std::unique_ptr<VAO> vao;
		public:
			SingleBuffer() :GLRenderer() {
				positionVbo = std::make_unique<VBO>();
				positionVbo->setData(positions);
				colorVbo = std::make_unique<VBO>();
				colorVbo->setData(colors);

				vao = std::make_unique<VAO>();

				vao->set(*positionVbo, 3, BUFF_ATTRIBUTION::VERT_POSITION);

				vao->set(*colorVbo, 3, BUFF_ATTRIBUTION::COLOR);
			}
			virtual ~SingleBuffer() = default;
			void operator()() override {

			}
		};

		export class InterLeavedBuffer : public GLRenderer {
			std::unique_ptr<VBO> posColorVbo;
			std::unique_ptr<VAO> vao;
			std::unique_ptr<LoadShader> loadShader;
		public:
			InterLeavedBuffer() :GLRenderer() {
				posColorVbo = std::make_unique<VBO>();
				posColorVbo->setData(pos_col_interleaved);

				vao = std::make_unique<VAO>();

				vao->set(*posColorVbo, 3, 6, 0, BUFF_ATTRIBUTION::VERT_POSITION);

				vao->set(*posColorVbo, 3, 6, 3, BUFF_ATTRIBUTION::COLOR);

				ShaderSrcs shaders = {
					{SHADER_TYPE::VERTEX,{"shaders\\beginner\\shader.vert"}},
					{SHADER_TYPE::FRAGMENT,{"shaders\\beginner\\shader.frag"}}
				};

				loadShader = std::make_unique<LoadShader>(shaders);

			}
			void operator()() override {
				loadShader->useProgram();
				glCall(glBindVertexArray, *vao);
				glCall(glDrawArrays, GL_TRIANGLES, 0, 3);
			}

		};

	}
}
