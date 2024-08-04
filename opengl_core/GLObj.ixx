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
#include <string>
#include "VertexDescriptor.hpp"
#include <boost/hana/tuple.hpp>
#include <boost/hana/for_each.hpp>
#include <iostream>
export module GLObjWrapper;

import GLCheckError;

namespace RGL {
	namespace glcore {

		//VBO,vertex buffer object，CPU存储在内存里，对于显存一段区域的描述，所以描述的是内存本身，不包括内存数据的描述
		//class VBO{ID,GPU_ADDRESS,SIZE,etc.}；
		//对于VBO的操作都需要先绑定，再操作
		export class VBO {
			std::unique_ptr<GLuint[]> vbo;
			GLuint mNumOfVbo;
			spdlog::logger* logger;

		public:
			VBO(GLuint numOfVbo) :mNumOfVbo(numOfVbo) {
				//genBuffer没有分配显存,仅仅是创建vbo
				vbo = std::make_unique<GLuint[]>(mNumOfVbo);
				logger = Logger::getInstance();
				glCall(glCreateBuffers, mNumOfVbo, vbo.get());

			}
			//长度为1的封装
			VBO() :VBO(1) {
			}
			~VBO()
			{
				glCall(glBindBuffer, GL_ARRAY_BUFFER, 0);
				glCall(glDeleteBuffers, mNumOfVbo, vbo.get());

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
				//glCall(glBindBuffer, GL_ARRAY_BUFFER,);

				if (!glIsBuffer(vbo[vboIdx])) {
					logger->error("Index {} is not a valid vbo object.", vboIdx);
				}

				//最后一个参数代表传入的数据没什么变化，提供驱动优化策略，还有一个是DYNAMIC_DRAW
				glCall(glNamedBufferData, vbo[vboIdx], data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
			}
			//长度为1时发送一个数据给GPU
			void setData(const std::vector<float>& data) {
				assert(mNumOfVbo == 1);
				setData(0, data);
			}
		};

		//glDrawArrays TRIANGLES不能复用边，但是TRIANGLE STRIP/FAN过于死板，所以需要使用顶点索引来让TRIANGLES拥有复用边的能力
//将顶点索引进行排列之后，可以对现有顶点进行任意组合，形成多种形状
//在单纯顶点的情况下，使用索引并不能节约空间，但是顶点除了位置以外，还有颜色，法线，UV等信息，这种情况下，顶点就又灵活又节约空间
//EBO(Element buffer object)用于储存顶点索引编号的**显存区域**，又一个烂名字

		export class EBO {
			std::unique_ptr<GLuint[]> ebo;
			GLuint mNumOfEbo;

		public:
			EBO(GLuint numOfEbo) :mNumOfEbo(numOfEbo) {
				ebo = std::make_unique<GLuint[]>(mNumOfEbo);

				//创建mNumOfEbo个ebo，未分配显存
				//glGenBuffers,mNumOfEbo, ebo.get());
				glCall(glCreateBuffers, mNumOfEbo, ebo.get());
			}

			EBO() :EBO(1) { }

			/// <summary>
			/// 给特定index上的ebo初始化数据，发送数据到显存
			/// </summary>
			/// <param name="eboIdx"></param>
			/// <param name="data">int数组，存储了三角形的顶点序号</param>
			void setData(GLuint eboIdx, const std::vector<GLint>& data) {
				assert(eboIdx < mNumOfEbo);
				//创建数据前先要绑定需要操作的ebo

				glNamedBufferData(ebo[eboIdx], data.size() * sizeof(decltype(data[0])), data.data(), GL_STATIC_DRAW);
			}

			void setData(const std::vector<GLint>& data) {
				setData(0, data);
			}

			GLuint operator[](GLuint idx) {
				assert(idx < mNumOfEbo);
				return ebo[idx];
			}

			operator GLuint() {
				assert(mNumOfEbo == 1);
				return ebo[0];
			}

			~EBO() {
				//glCall(glDeleteBuffers,mNumOfEbo, static_cast<GLuint*>(ebo.get()));
				glCall(glDeleteBuffers, mNumOfEbo, static_cast<GLuint*>(ebo.get()));
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

		//VAO = vbo descriptor array object，用于描述vbo内各种buffer属性的数组，每个position描述一个buffer属性，烂名字


		export class VAO {
			std::unique_ptr<GLuint[]> vao;
			//所有vao的position，初始化为0，每次调用set自增1
			//是重要的和shader产生联系的属性，layout(location=n)的n代表第n个position

			GLuint mNumOfVao;


			spdlog::logger* logger;

			//到时候直接从shader program里查询不同顶点属性的layout location
			std::optional<GLuint> shaderProgram;

		public:
			VAO(size_t numOfVao) :mNumOfVao(numOfVao), shaderProgram{} {
				vao = std::make_unique<GLuint[]>(mNumOfVao);
				glCall(glCreateVertexArrays, mNumOfVao, vao.get());

				logger = Logger::getInstance();

			}
			VAO() :VAO(1) {}
			~VAO() {
				glCall(glBindVertexArray, 0);
				glCall(glDeleteVertexArrays, mNumOfVao, vao.get());
			}

			void setShaderProgram(GLuint shader) {
				if (!shaderProgram.has_value())
				{
					shaderProgram = shader;
				}
				else
				{
					throw GLLogicError("Shader program has already has value");
				}
			}

			/// <summary>
			/// 对应single buffer状态（一个VBO对应一个属性）的VAO属性设置封装
			/// </summary>
			/// <param name="vaoIdx">设置vaoIdx位置上的属性，同时将其绑定到输入vbo，因为可能一次性创建了多个vao，所以需要确定vao的下标</param>
			/// <param name="vbo">需要设置属性的vbo，可以是vbo编号，也可以是VBO对象</param>
			/// <param name="numOfFloat">该属性的float数量，顶点坐标可能是3个，RGBA颜色可能是4个，UV纹理坐标可能是2个</param>
			/// <param name="shaderInputName"></param>
			void set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat, const std::string& shaderInputName) {
				assert(vaoIdx < mNumOfVao);
				if (!glIsBuffer(vbo)) {
					logger->error("Index {} is not a valid vbo object.", vbo);
				}

				//要设置的是vao[vaoIdx]，所以绑定它
				glCall(glBindVertexArray, vao[vaoIdx]);
				//因为要vao是vbo的属性，所以先绑定vbo，再设置vao
				glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo);

				const auto location = glCallRet(glGetAttribLocation, shaderProgram.value(), shaderInputName.c_str());
				glCheckError();

				glCall(glEnableVertexAttribArray, location);

				glCall(glVertexAttribPointer, location, numOfFloat, GL_FLOAT, GL_TRUE, numOfFloat * sizeof(float), (void*)nullptr);


			}

			/// <summary>
			/// 当vao只有一个的时候的single buffer简化设置
			/// </summary>
			/// <param name="vbo"></param>
			/// <param name="numOfFloat"></param>
			/// <param name="shaderInputName"></param>
			void set(GLuint vbo, GLuint numOfFloat, const std::string& shaderInputName) {
				assert(mNumOfVao == 1);
				set(0, vbo, shaderInputName);
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
			/// <param name="shaderInputName"></param>
			void set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat, GLuint stride, GLuint offset, const std::string& shaderInputName) {
				assert(vaoIdx < mNumOfVao);
				glCall(glBindVertexArray, vao[vaoIdx]);
				glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo);

				const auto location = glCallRet(glGetAttribLocation, shaderProgram.value(), shaderInputName.c_str());


				glCall(glEnableVertexAttribArray, location);

				glCall(glVertexAttribPointer, location,	//设置vaoPos位置上的属性
					numOfFloat,					//属性由几个float组成，一个顶点可能是3个float，一个颜色可能是三个或者四个float（RGB或RGBA）
					GL_FLOAT,					//属性由几个float组成
					GL_TRUE,					//属性归一化了
					stride * sizeof(float),	//相邻vbo元素之间的跨度，就是单个vbo的大小（stride）
					(void*)(offset * sizeof(float)));				//vbo内部跨度，该属性是单个vbo起始地址的偏移量（offset)

			}
			//VertexDescType是一个包含了顶点属性的元组，元组成员拥有获取属性长度，size，以及命名的能力
			//假设有VertexDescType desc;desc可以.name，getLength()，getSize()
			template<IsVertexElementTuple VertexDescType>
			void setDSA_interleaved(const GLuint vaoIdx, const GLuint vbo, const VertexDescType& vertexDescription) {
				size_t size = 0;
				hana::for_each(vertexDescription, [&size](auto vert) {
					size += vert.getSize();
					});
				//关联vbo和vao,设置vertex总大小
				
				glCall(glVertexArrayVertexBuffer,vao[vaoIdx], 0, vbo, 0, size);


				size_t current_offset = 0;
				// 编译期遍历顶点属性元组，计算offset
				hana::for_each(vertexDescription, [this,&current_offset,  &vaoIdx, &vbo](auto vert) {

					//std::cout << vert.name << " offset: " << current_offset << ". Length:  " << vert.getLength() << std::endl;
					const std::string shaderInputName = vert.name;
					GLuint location = glCallRet(glGetAttribLocation, shaderProgram.value(), shaderInputName.c_str());
					const GLuint length = vert.getLength();
					//首先在相应的vertex shader的layout location上激活vao属性
					glCall(glEnableVertexArrayAttrib, vao[vaoIdx], location);
					//设置顶点描述
					glCall(glVertexArrayAttribFormat, vao[vaoIdx], location, length, GL_FLOAT, GL_TRUE, current_offset);
					//绑定vao,因为交错式缓冲区所有数据公用一块内存，所以binding index都为0
					glCall(glVertexArrayAttribBinding, vao[vaoIdx], location, 0);

					current_offset += vert.getSize();//累加size以更新offset
					});
			}

			template<IsVertexElementTuple VertexDescType>
			void setDSA_interleaved(const GLuint vbo, const VertexDescType& vertexDescription) {
				setDSA_interleaved(0, vbo, vertexDescription);
			}
			/// <summary>
			/// 当VAO长度为1的简化
			/// </summary>
			/// <param name="vbo"></param>
			/// <param name="numOfFloat"></param>
			/// <param name="stride"></param>
			/// <param name="offset"></param>
			/// <param name="shaderInputName"></param>
			void set(GLuint vbo, GLuint numOfFloat, GLuint stride, GLuint offset, const std::string& shaderInputName) {
				assert(mNumOfVao == 1);
				set(0, vbo, numOfFloat, stride, offset, shaderInputName);
			}



			/// <summary>
			/// 先确定哪个vao需要操作（绑定）,然后把ebo绑定到这个vao上
			/// </summary>
			/// <param name="vaoIdx"></param>
			/// <param name="ebo"></param>
			void addEBO(GLuint vaoIdx, GLuint ebo) {
				glCall(glBindVertexArray, vao[vaoIdx]);
				glCall(glBindBuffer, GL_ELEMENT_ARRAY_BUFFER, ebo);
			}
			void addEBO(GLuint ebo) {
				assert(mNumOfVao == 1);
				addEBO(0, ebo);
			}

		};




	}


}
