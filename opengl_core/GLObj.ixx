module;
#include <glad/glad.h>
#include "Helpers.hpp"
#include <cassert>
#include <vector>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <map>
#include <vector>
#include <utility>
export module GLObjWrapper;

import GLCheckError;

namespace RGL {
	namespace glcore {





		//VBO,vertex buffer object，CPU存储在内存里，对于显存一段区域的描述
		//class VBO{ID,GPU_ADDRESS,SIZE,etc.}；
		//对于VBO的操作都需要先绑定，再操作
		export class VBO {
			GLuint* vbo;
			GLuint mNumOfVbo;
			spdlog::logger* logger;
			VBO(GLuint numOfVbo) :mNumOfVbo(numOfVbo) {
				//genBuffer没有分配显存,仅仅是创建vbo
				vbo = new GLuint[mNumOfVbo];
				logger = Logger::getInstance();
				glCall(glGenBuffers, mNumOfVbo, vbo);

				for (size_t i = 0; i < mNumOfVbo; i++)
				{
					glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo[i]);
					if (!glIsBuffer(vbo[i])) {
						logger->error("Index {} is not a valid vbo object.", vbo[i]);
					}
				}
			}


		public:

			//长度为1的封装
			VBO() :VBO(1) {
			}
			~VBO()
			{
				glCall(glDeleteBuffers, mNumOfVbo, vbo);
				delete[] vbo;
			}
			//根据index获取vbo
			GLuint operator[](GLuint i) {
				assert(i < mNumOfVbo);
				return vbo[i];
			}
			//当长度为1时直接返回vbo
			GLuint operator()() {
				assert(mNumOfVbo == 1);
				return vbo[0];
			}

			//当长度为1时也可以直接和vbo GLuint隐式转换
			operator GLuint() const {
				assert(mNumOfVbo == 1);
				return vbo[0];
			}

			//使用glBufferData，往GPU发送数据，开辟空间。
			//每次调用都会开辟显存，不要频繁调用
			//调用前先绑定
			void setData(GLuint vboIdx, const std::vector<float>& data) {
				assert(vboIdx < mNumOfVbo);
				glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo[vboIdx]);

				if (!glIsBuffer(vbo[vboIdx])) {
					logger->error("Index {} is not a valid vbo object.", vboIdx);
				}

				//最后一个参数代表传入的数据没什么变化，提供驱动优化策略，还有一个是DYNAMIC_DRAW
				glCall(glBufferData, GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
			}
			//长度为1时发送一个数据给GPU
			void setData(const std::vector<float>& data) {
				assert(mNumOfVbo == 1);
				setData(0, data);
			}
		};



		//VBO本质上就是一堆数据+数据区域描述，对这些数据添加额外的描述（顶点，颜色，等等），让GPU理解数据的作用，需要VAO
		// 定义：用于储存一个Mesh网格所有的顶点属性描述信息
		//以顶点为例
		//size->每个顶点3个数字
		//type->顶点数据类型float
		//stride->偏移量，每个顶点所占大小3*sizeof(float)，如果是interleaved（顶点，颜色，顶点，颜色，不断交叉）数据，这个数据比较重要
		//其他描述信息：法线，uv(纹理位置)等等
		//VBO可能是这些数据，VAO要对这些数据进行描述
		/*
			struct VAO_ELEMENT {
				float position[3];
				float color[3];
				float normal[3];
				float uv[3];
			};
		*/

		//VAO = vbo descriptor array object

		export enum class BUFF_ATTRIBUTION
		{
			VERT_POSITION,
			COLOR,
			UV,
		};

		using PositionBuffType = std::vector<BUFF_ATTRIBUTION>;
		using VAOs_Pos_Types = std::map<GLuint, PositionBuffType>;

		export class VAO {
			GLuint* vao;
			//所有vao的position，初始化为0，每次调用set自增1
			//是重要的和shader产生联系的属性，layout(location=n)的n代表第n个position
			GLuint* positions;
			GLuint mNumOfVao;
			VAOs_Pos_Types vao_pos_types;
			VAO(size_t numOfVao) :mNumOfVao(numOfVao), vao_pos_types() {
				vao = new GLuint[mNumOfVao];
				glGenVertexArrays(mNumOfVao, vao);
				logger = Logger::getInstance();
				positions = new GLuint[mNumOfVao];
				for (size_t i = 0; i < mNumOfVao; i++)
				{
					positions[i] = 0;
				}
			}
			spdlog::logger* logger;

			bool checkAttributionDuplicated(GLuint vaoIdx, BUFF_ATTRIBUTION attrib) {
				const auto& currentAttributes = vao_pos_types[vaoIdx];
				if (std::find(currentAttributes.begin(), currentAttributes.end(), attrib) != currentAttributes.end()) {
					return true;
				}
				else
				{
					return false;
				}
			}

		public:

			VAO() :VAO(1) {}
			~VAO() {
				delete[] vao;
				delete[] positions;
			}


			/// <summary>
			/// 对应single buffer状态（一个VBO对应一个属性）的VAO属性设置封装
			/// </summary>
			/// <param name="vaoIdx">设置vaoIdx位置上的属性，同时将其绑定到输入vbo，因为可能一次性创建了多个vao，所以需要确定vao的下标</param>
			/// <param name="vbo">需要设置属性的vbo，可以是vbo编号，也可以是VBO对象</param>
			/// <param name="numOfFloat">该属性的float数量，顶点坐标可能是3个，RGBA颜色可能是4个，UV纹理坐标可能是2个</param>
			/// <param name="buffAttr">VAO对应VBO属性类型枚举，方便通过属性查看VAO的Position，本类使用了一个map储存了position和属性的表</param>
			void set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat, BUFF_ATTRIBUTION buffAttr) {
				assert(vaoIdx < mNumOfVao);
				if (!glIsBuffer(vbo)) {
					logger->error("Index {} is not a valid vbo object.", vbo);
				}

				//要设置的是vao[vaoIdx]，所以绑定它
				glCall(glBindVertexArray, vao[vaoIdx]);
				//因为要vao是vbo的属性，所以先绑定vbo，再设置vao
				glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo);
				glCall(glEnableVertexAttribArray, positions[vaoIdx]);

				glCall(glVertexAttribPointer, positions[vaoIdx],numOfFloat,	GL_FLOAT,GL_TRUE,numOfFloat * sizeof(float),(void*)NULL);
				if (checkAttributionDuplicated(vaoIdx, buffAttr))
				{
					this->logger->error("Input duplated attribution");
				}
				vao_pos_types[vaoIdx].push_back(buffAttr);

				positions[vaoIdx]++;//每次调用后position+1
			}

			/// <summary>
			/// 当vao只有一个的时候的single buffer简化设置
			/// </summary>
			/// <param name="vbo"></param>
			/// <param name="numOfFloat"></param>
			/// <param name="buffAttr"></param>
			void set(GLuint vbo, GLuint numOfFloat, BUFF_ATTRIBUTION buffAttr) {
				assert(mNumOfVao == 1);
				set(0, vbo, numOfFloat, buffAttr);
			}

			operator GLuint() {
				assert(mNumOfVao == 1);
				return vao[0];
			}

			/// <summary>
			/// 适用于interleaved buffer的
			/// </summary>
			/// <param name="vaoIdx">设置vaoIdx位置上的属性，同时将其绑定到输入vbo，因为可能一次性创建了多个vao，所以需要确定vao的下标</param>
			/// <param name="vbo">需要设置属性的vbo，可以是vbo编号，也可以是VBO对象</param>
			/// <param name="numOfFloat">该属性的float数量，顶点坐标可能是3个，RGBA颜色可能是4个，UV纹理坐标可能是2个</param>
			/// <param name="stride">每个vbo里有多少个float，假设每个vbo有一个顶点和一个rgba颜色，那么每个顶点有3个float（zyz）+4个颜色float（rgba），那么这里写7</param>
			/// <param name="offset">这次set相对这个vbo首地址的偏移量，假设vbo排列是float*3（顶点）+float*4（颜色），那么在设置颜色的时候，offset就是3</param>
			/// <param name="buffAttr">VAO对应VBO属性类型枚举，方便通过属性查看VAO的Position，本类使用了一个map储存了position和属性的表</param>
			void set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat, GLuint stride, GLuint offset, BUFF_ATTRIBUTION buffAttr) {
				assert(vaoIdx < mNumOfVao);
				glCall(glBindVertexArray, vao[vaoIdx]);
				glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo);
				glCall(glEnableVertexAttribArray, positions[vaoIdx]);

				glCall(glVertexAttribPointer, positions[vaoIdx],	//设置vaoPos位置上的属性
					numOfFloat,					//属性由几个float组成，一个顶点可能是3个float，一个颜色可能是三个或者四个float（RGB或RGBA）
					GL_FLOAT,					//属性由几个float组成
					GL_TRUE,					//属性归一化了
					stride * sizeof(float),	//相邻vbo元素之间的跨度，就是单个vbo的大小（stride）
					(void*)(offset * sizeof(float)));				//vbo内部跨度，该属性是单个vbo起始地址的偏移量（offset)

				if (checkAttributionDuplicated(vaoIdx,buffAttr))
				{
					this->logger->error("Input duplated attribution");
				}

				vao_pos_types[vaoIdx].push_back(buffAttr);

				positions[vaoIdx]++;
			}

			void set(GLuint vbo, GLuint numOfFloat, GLuint stride, GLuint offset, BUFF_ATTRIBUTION buffAttr) {
				assert(mNumOfVao == 1);
				set(0, vbo, numOfFloat, stride, offset, buffAttr);
			}
			/// <summary>
			/// 帮助shader的layout(location = N)定位不同attribution的location
			/// </summary>
			/// <param name="vaoIdx">vaoIdx</param>
			/// <param name="attr">需要查找location的顶点属性</param>
			/// <returns></returns>
			GLuint getPositionByAttribution(GLuint vaoIdx, BUFF_ATTRIBUTION attr) {
				const auto it = std::find(vao_pos_types[vaoIdx].begin(), vao_pos_types[vaoIdx].end(), attr);
				if (it != vao_pos_types[vaoIdx].end())
				{
					return std::distance(vao_pos_types[vaoIdx].begin(), it);
				}
				else {
					this->logger->error("Cannot find attr");
				}
			}
			/// <summary>
			/// 帮助shader的layout(location = N)定位不同attribution的location
			/// </summary>
			/// <param name="attr">需要查找location的顶点属性</param>
			/// <returns></returns>
			GLuint getPositionByAttribution(BUFF_ATTRIBUTION attr) {
				return getPositionByAttribution(0, attr);
			}
		};
	}
}
