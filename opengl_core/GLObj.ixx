module;
#include <glad/glad.h>
#include "Helpers.hpp"
#include <cassert>
#include <vector>
#include <stdexcept>
#include <spdlog/spdlog.h>
export module GLObjWrapper;

import GLCheckError;

namespace RGL {
	namespace glcore {


		//VBO,vertex buffer object，CPU存储在内存里，对于显存一段区域的描述
		//class VBO{ID,GPU_ADDRESS,SIZE,etc.}；
		//对于VBO的操作都需要先绑定，再操作，再解除绑定
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
		//其他描述信息：法线，uv等等
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
		export class VAO {
			GLuint* vao;
			//所有vao的position，初始化为0，每次调用set自增1
			GLuint* positions;
			GLuint mNumOfVao;
			VAO(size_t numOfVao) :mNumOfVao(numOfVao) {
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
		public:

			VAO() :VAO(1) {}
			~VAO() {
				delete[] vao;
				delete[] positions;
			}

			//设置vaoIdx位置上的属性，同时将其绑定到输入vbo
			//因为一个vao有多个描述位，所以需要设定描述位的index（vaoPos）
			//本函数对应single buffer状态（一个VBO对应一个属性）
			void set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat) {
				assert(vaoIdx < mNumOfVao);
				if (!glIsBuffer(vbo)) {
					logger->error("Index {} is not a valid vbo object.", vbo);
				}

				//要设置的是vao[vaoIdx]，所以绑定它
				glCall(glBindVertexArray, vao[vaoIdx]);
				//因为要vao是vbo的属性，所以先绑定vbo，再设置vao
				glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo);
				glCall(glEnableVertexAttribArray, positions[vaoIdx]);

				glCall(glVertexAttribPointer, positions[vaoIdx],	//设置vaoPos位置上的属性
					numOfFloat,					//属性由几个float组成，一个顶点可能是3个float，一个颜色可能是三个或者四个float（RGB或RGBA）
					GL_FLOAT,					//属性由几个float组成
					GL_TRUE,					//属性归一化了
					numOfFloat * sizeof(float),	//相邻vbo元素之间的跨度，就是单个vbo的大小（stride）
					(void*)NULL);				//vbo内部跨度，该属性是单个vbo起始地址的偏移量（offset）

				positions[vaoIdx]++;//每次调用后position+1
			}
			//本函数对应single buffer状态（一个VBO对应一个属性）
			void set(GLuint vbo, GLuint numOfFloat) {
				assert(mNumOfVao == 1);
				set(0, vbo, numOfFloat);
			}

			//stride:每个vbo里有多少个float，假设每个vbo有一个顶点和一个rgba颜色，那么每个顶点有3个float（zyz）+4个颜色float（rgba），那么这里写7
			//offset：这次set相对这个vbo首地址的偏移量，假设vbo排列是float*3（顶点）+float*4（颜色），那么在设置颜色的时候，offset就是3
			void set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat, GLuint stride, GLuint offset) {
				assert(vaoIdx < mNumOfVao);
				glCall(glBindVertexArray, vao[vaoIdx]);
				glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo);
				glCall(glEnableVertexAttribArray, positions[vaoIdx]);

				glCall(glVertexAttribPointer, positions[vaoIdx],	//设置vaoPos位置上的属性
					numOfFloat,					//属性由几个float组成，一个顶点可能是3个float，一个颜色可能是三个或者四个float（RGB或RGBA）
					GL_FLOAT,					//属性由几个float组成
					GL_TRUE,					//属性归一化了
					stride * sizeof(float),	//相邻vbo元素之间的跨度，就是单个vbo的大小（stride）
					(void*)(offset * sizeof(float)));				//vbo内部跨度，该属性是单个vbo起始地址的偏移量（offset）
				positions[vaoIdx]++;
			}

			void set(GLuint vbo, GLuint numOfFloat, GLuint stride, GLuint offset) {
				assert(mNumOfVao == 1);
				set(0, vbo, numOfFloat, stride, offset);
			}
		};
	}
}