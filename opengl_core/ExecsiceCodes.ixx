module;
export module ExecsiceCodes;
#include <vector>
#include <memory>
import GLObjWrapper;

import GLFramework;
namespace RGL {
	namespace glcore {
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

				//��Ӧsingle buffer����������vao�ķ�װ����vao��vbo��
				vao->set(*positionVbo,3);

				vao->set(*colorVbo, 3);

			}
			virtual ~SingleBuffer() = default;
			void operator()() override {

			}
		};

	}
}