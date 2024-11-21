#pragma once
#include "Helpers.hpp"
#include <cassert>
#include <cstddef>
#include <glad/glad.h>
#include <memory>
#include <spdlog/spdlog.h>

#include <vector>

#include "VertexDescriptor.hpp"
#include <boost/pfr.hpp>
#include <string>
#include <concepts>
#include "GLCheckError.hpp"

namespace RGL {
namespace glcore {

// 顶点数据和顶点索引放在同一个结构体里
struct VerticesWithIndices {
    std::vector<float> vertices;
    std::vector<int> indices;
};

// VBO,vertex buffer
// object，CPU存储在内存里，对于显存一段区域的描述，所以描述的是内存本身，不包括内存数据的描述
// class VBO{ID,GPU_ADDRESS,SIZE,etc.}；
// 对于VBO的操作都需要先绑定，再操作
class VBO {
    std::unique_ptr<GLuint[]> vbo;
    std::unique_ptr<bool[]> withIndices;
    std::unique_ptr<size_t[]> verticesSizes;  // vbo的顶点的大小，即顶点数量*每个顶点的大小

    GLuint mNumOfVbo;
    spdlog::logger *logger;

   public:
    GLuint getSize() const;

    bool getWithIndices(GLuint vboIdx) const;
    size_t getVerticesSize(GLuint vboIdx) const;
    size_t getVerticesSize() const;
    VBO(GLuint numOfVbo);
    // 长度为1的封装
    VBO();
    ~VBO();
    // 根据index获取vbo
    GLuint operator[](GLuint i) const;
    // 当长度为1时直接返回vbo
    GLuint operator()();

    // 当长度为1时也可以直接和vbo GLuint隐式转换
    operator GLuint() const;

    // 使用glBufferData，往GPU发送数据，开辟空间。
    // 每次调用都会开辟显存，不要频繁调用
    // 调用前先绑定
    void setData(GLuint vboIdx, const std::vector<float> &data);
    // 长度为1时发送一个数据给GPU
    void setData(const std::vector<float> &data);

    // 将顶点数据和顶点索引同时发送到一个VBO内
    // 顶点数据排布在顶点索引之前
    void setData(GLuint vboIdx, const VerticesWithIndices &verticesWithIndices);
    void setData(const VerticesWithIndices &verticesWithIndices);


	//如果数据类型是GLuint，那么VBO就是EBO
	void setData(GLuint eboIdx, const std::vector<GLint> &data);
    void setData(const std::vector<GLint> &data);


};

using EBO = VBO;


// glDrawArrays TRIANGLES不能复用边，但是TRIANGLE
// STRIP/FAN过于死板，所以需要使用顶点索引来让TRIANGLES拥有复用边的能力
// 将顶点索引进行排列之后，可以对现有顶点进行任意组合，形成多种形状
// 在单纯顶点的情况下，使用索引并不能节约空间，但是顶点除了位置以外，还有颜色，法线，UV等信息，这种情况下，顶点就又灵活又节约空间
// EBO(Element buffer object)用于储存顶点索引编号的**显存区域**，又一个烂名字



// VBO本质上就是一堆数据+数据区域描述，对这些数据添加额外的描述（顶点，颜色，等等），让GPU理解数据的作用，需要VAO
//  定义：用于储存一个Mesh网格所有的顶点属性描述信息
// 以顶点为例
// size->每个顶点3个数字
// type->顶点数据类型float
// stride->偏移量，每个顶点所占大小3*sizeof(float)，如果是interleaved（顶点，颜色，顶点，颜色，不断交叉）数据，这个数据比较重要
// 其他描述信息：法线，uv(纹理位置)等等
// VBO可能是这些数据，VAO要对这些数据进行描述
/*
	struct VAO_ELEMENT {
		float position[3];
		float color[3];
		float normal[3];
		float uv[3];
	};
*/

// VAO = vbo descriptor array
// object，用于描述vbo内各种buffer属性的数组，每个position描述一个buffer属性，烂名字

class VAO {
    std::unique_ptr<GLuint[]> vao;
    // 所有vao的position，初始化为0，每次调用set自增1
    // 是重要的和shader产生联系的属性，layout(location=n)的n代表第n个position

    GLuint mNumOfVao;

    spdlog::logger *logger;

    // 到时候直接从shader program里查询不同顶点属性的layout location
    GLuint shaderProgram;

   public:
    VAO(size_t numOfVao);
    VAO() : VAO(1) {}
    ~VAO();

    void setShaderProgram(GLuint shader);

    /// <summary>
    /// 对应single
    /// buffer状态（一个VBO对应一个属性）的VAO属性设置封装
    /// </summary>
    /// <param
    /// name="vaoIdx">设置vaoIdx位置上的属性，同时将其绑定到输入vbo，因为可能一次性创建了多个vao，所以需要确定vao的下标</param>
    /// <param
    /// name="vbo">需要设置属性的vbo，可以是vbo编号，也可以是VBO对象</param>
    /// <param
    /// name="numOfFloat">该属性的float数量，顶点坐标可能是3个，RGBA颜色可能是4个，UV纹理坐标可能是2个</param>
    /// <param name="shaderInputName"></param>
    void set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat,
	const std::string &shaderInputName);

    /// <summary>
    /// 当vao只有一个的时候的single buffer简化设置
    /// </summary>
    /// <param name="vbo"></param>
    /// <param name="numOfFloat"></param>
    /// <param name="shaderInputName"></param>
    void set(GLuint vbo, GLuint numOfFloat, const std::string &shaderInputName);

    operator GLuint();
    GLuint operator[](int idx) {
	return vao[idx];
    }
    /// <summary>
    /// 适用于interleaved buffer的
    /// </summary>
    /// <param
    /// name="vaoIdx">设置vaoIdx位置上的属性，同时将其绑定到输入vbo，因为可能一次性创建了多个vao，所以需要确定vao的下标</param>
    /// <param
    /// name="vbo">需要设置属性的vbo，可以是vbo编号，也可以是VBO对象</param>
    /// <param
    /// name="numOfFloat">该属性的float数量，顶点坐标可能是3个，RGBA颜色可能是4个，UV纹理坐标可能是2个</param>
    /// <param
    /// name="stride">每个vbo里有多少个float，假设每个vbo有一个顶点和一个rgba颜色，那么每个顶点有3个float（zyz）+4个颜色float（rgba），那么这里写7</param>
    /// <param
    /// name="offset">这次set相对这个vbo首地址的偏移量，假设vbo排列是float*3（顶点）+float*4（颜色），那么在设置颜色的时候，offset就是3</param>
    /// <param name="shaderInputName"></param>
    void set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat, GLuint stride,
	GLuint offset, const std::string &shaderInputName);
    // VertexDescType是一个包含了顶点属性的元组，元组成员拥有获取属性长度，size，以及命名的能力
    // 假设有VertexDescType
    // desc;desc可以.name，getLength()，getSize()
    template <IsVertexElementTuple VertexDescType>
    void setDSA_interleaved(const GLuint vaoIdx, const GLuint vbo,
	const VertexDescType &vertexDescription) {
	size_t size = 0;
	////累加总size
	hana::for_each(vertexDescription,
	    [&size](auto vert) { size += vert.getSize(); });
	// 关联vbo和vao,设置vertex总大小，设置binding
	// index为0，所以下面的绑定点也是0，毕竟interleaved，一个绑定点就够了。
	glCall(glVertexArrayVertexBuffer, vao[vaoIdx], 0  // binding index
	    ,
	    vbo, 0  // vbo数据的offset，如果vbo一开始有顶点索引或者为了保证对其的话，不是0
	    ,
	    size);  // 每个顶点数据的大小
	assert(glIsProgram(this->shaderProgram));
	size_t current_offset = 0;
	// 编译期遍历顶点属性元组，计算offset
	hana::for_each(vertexDescription, [this, &current_offset, &vaoIdx,
					      &vbo](auto vert) {
	    std::string_view shaderInputName = vert.name;
	    const GLint location = glCallRet(glGetAttribLocation, shaderProgram,
		std::string(shaderInputName).c_str());
	    if (location == -1) {
		logger->error("shader input {} not found.\nThis is an optimized value or wrong input or gl_preserved name", std::string(shaderInputName));
	    } else {
		constexpr GLuint length = vert.getLength();
		// 首先在相应的vertex shader的layout location上激活vao属性
		glCall(glEnableVertexArrayAttrib, vao[vaoIdx], location);
		// 设置顶点描述
		glCall(glVertexArrayAttribFormat, vao[vaoIdx], location, length, GL_FLOAT,
		    GL_TRUE, current_offset);
		// 绑定到vao的绑定点上,对于interleaved
		// buffer，就一个buffer，上面已经设置了绑定点为0，所以这里都是0
		glCall(glVertexArrayAttribBinding, vao[vaoIdx], location, 0);
		current_offset += vert.getSize();  // 累加size以更新offset
	    }
	});
    }

    template <IsVertexElementTuple VertexDescType>
    void setDSA_interleaved(const GLuint vbo,
	const VertexDescType &vertexDescription) {
	setDSA_interleaved(0, vbo, vertexDescription);
    }

    void setDSA_interleaved(const GLuint vaoIdx, const GLuint vbo, FloatDescs descs);

    /// <summary>
    /// 当VAO长度为1的简化
    /// </summary>
    /// <param name="vbo"></param>
    /// <param name="numOfFloat"></param>
    /// <param name="stride"></param>
    /// <param name="offset"></param>
    /// <param name="shaderInputName"></param>
    void set(GLuint vbo, GLuint numOfFloat, GLuint stride, GLuint offset,
	const std::string &shaderInputName);

    /// <summary>
    /// 先确定哪个vao需要操作（绑定）,然后把ebo绑定到这个vao上
    /// </summary>
    /// <param name="vaoIdx"></param>
    /// <param name="ebo"></param>
    void addEBO(GLuint vaoIdx, GLuint ebo);
    void addEBO(GLuint ebo);
};
}  // namespace glcore
}  // namespace RGL
