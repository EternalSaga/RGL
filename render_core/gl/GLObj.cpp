#include "GLObj.hpp"

#include <cstddef>
#include <memory>
#include "GLCheckError.hpp"
#include "rllogger.hpp"
namespace RGL {
	namespace glcore {

        VBO::VBO(GLuint numOfVbo) : mNumOfVbo(numOfVbo) {
          // genBuffer没有分配显存,仅仅是创建vbo
          vbo = std::make_unique<GLuint[]>(mNumOfVbo);
          withIndices = std::make_unique<bool[]>(mNumOfVbo);
          verticesSizes = std::make_unique<GLuint[]>(mNumOfVbo);
          for (size_t i = 0; i < mNumOfVbo; i++) {
            withIndices[i] = false;
            verticesSizes[i] = 0;
          }
          
          logger = RGL::RLLogger::getInstance();

          glCall(glCreateBuffers, mNumOfVbo, vbo.get());
        }
        VBO::VBO() : VBO(1) {}
        VBO::~VBO() {
          glCall(glBindBuffer, GL_ARRAY_BUFFER, 0);
          glCall(glDeleteBuffers, mNumOfVbo, vbo.get());
        }
        GLuint VBO::operator[](GLuint i) const{
          assert(i < mNumOfVbo);
          return vbo[i];
        }
        GLuint VBO::operator()() {
          assert(mNumOfVbo == 1);
          return vbo[0];
        }
        VBO::operator GLuint() const {
          assert(mNumOfVbo == 1);
          return vbo[0];
        }
        void VBO::setData(GLuint vboIdx, const std::vector<float> &data) {
          assert(vboIdx < mNumOfVbo);

          if (!glIsBuffer(vbo[vboIdx])) {
            logger->error("Index {} is not a valid vbo object.", vboIdx);
          }

          // 最后一个参数代表传入的数据没什么变化，提供驱动优化策略，还有一个是DYNAMIC_DRAW
          glCall(glNamedBufferData, vbo[vboIdx], data.size() * sizeof(float),
                 data.data(), GL_STATIC_DRAW);
        }
        void VBO::setData(const std::vector<float> &data) {
          assert(mNumOfVbo == 1);
          setData(0, data);
        }
        EBO::EBO(GLuint numOfEbo) : mNumOfEbo(numOfEbo) {
          ebo = std::make_unique<GLuint[]>(mNumOfEbo);

          // 创建mNumOfEbo个ebo，未分配显存
          // glGenBuffers,mNumOfEbo, ebo.get());
          glCall(glCreateBuffers, mNumOfEbo, ebo.get());
        }
        EBO::EBO() : EBO(1) {}
        void EBO::setData(GLuint eboIdx, const std::vector<GLint> &data) {
          assert(eboIdx < mNumOfEbo);
          // 创建数据前先要绑定需要操作的ebo

          glNamedBufferData(ebo[eboIdx],
                            data.size() * sizeof(decltype(data[0])),
                            data.data(), GL_STATIC_DRAW);
        }
        void EBO::setData(const std::vector<GLint> &data) { setData(0, data); }
        GLuint EBO::operator[](GLuint idx) {
          assert(idx < mNumOfEbo);
          return ebo[idx];
        }
        EBO::operator GLuint() {
          assert(mNumOfEbo == 1);
          return ebo[0];
        }
        EBO::~EBO() {
          // glCall(glDeleteBuffers,mNumOfEbo,
          // static_cast<GLuint*>(ebo.get()));
          glCall(glDeleteBuffers, mNumOfEbo, static_cast<GLuint *>(ebo.get()));
        }
        VAO::VAO(size_t numOfVao) : mNumOfVao(numOfVao), shaderProgram{} {
          vao = std::make_unique<GLuint[]>(mNumOfVao);
          glCall(glCreateVertexArrays, mNumOfVao, vao.get());

          logger = RGL::RLLogger::getInstance();
        }
        VAO::~VAO() {
          glCall(glBindVertexArray, 0);
          glCall(glDeleteVertexArrays, mNumOfVao, vao.get());
        }
        void VAO::setShaderProgram(GLuint shader) {
          if (!shaderProgram.has_value()) {
            shaderProgram = shader;
          } else {
            throw GLLogicError("Shader program has already has value");
          }
        }
        void VAO::set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat,
                      const std::string &shaderInputName) {
          assert(vaoIdx < mNumOfVao);
          if (!glIsBuffer(vbo)) {
            logger->error("Index {} is not a valid vbo object.", vbo);
          }

          // 要设置的是vao[vaoIdx]，所以绑定它
          glCall(glBindVertexArray, vao[vaoIdx]);
          // 因为要vao是vbo的属性，所以先绑定vbo，再设置vao
          glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo);

          const auto location =
              glCallRet(glGetAttribLocation, shaderProgram.value(),
                        shaderInputName.c_str());
          glCheckError();

          glCall(glEnableVertexAttribArray, location);

          glCall(glVertexAttribPointer, location, numOfFloat, GL_FLOAT, GL_TRUE,
                 numOfFloat * sizeof(float), (void *)nullptr);
        }
        void VAO::set(GLuint vbo, GLuint numOfFloat,
                      const std::string &shaderInputName) {
          assert(mNumOfVao == 1);
          set(0, vbo, shaderInputName);
        }
        VAO::operator GLuint() {
          assert(mNumOfVao == 1);
          return vao[0];
        }
        void VAO::set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat,
                      GLuint stride, GLuint offset,
                      const std::string &shaderInputName) {
          assert(vaoIdx < mNumOfVao);
          glCall(glBindVertexArray, vao[vaoIdx]);
          glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo);

          const auto location =
              glCallRet(glGetAttribLocation, shaderProgram.value(),
                        shaderInputName.c_str());

          glCall(glEnableVertexAttribArray, location);

          glCall(
              glVertexAttribPointer,
              location,   // 设置vaoPos位置上的属性
              numOfFloat, // 属性由几个float组成，一个顶点可能是3个float，一个颜色可能是三个或者四个float（RGB或RGBA）
              GL_FLOAT,   // 属性由几个float组成
              GL_TRUE,    // 属性归一化了
              stride *
                  sizeof(
                      float), // 相邻vbo元素之间的跨度，就是单个vbo的大小（stride）
              (void
                   *)(offset *
                      sizeof(
                          float))); // vbo内部跨度，该属性是单个vbo起始地址的偏移量（offset)
        }
        void VAO::set(GLuint vbo, GLuint numOfFloat, GLuint stride,
                      GLuint offset, const std::string &shaderInputName) {
          assert(mNumOfVao == 1);
          set(0, vbo, numOfFloat, stride, offset, shaderInputName);
        }
        void VAO::addEBO(GLuint vaoIdx, GLuint ebo) {
          glCall(glVertexArrayElementBuffer,vao[vaoIdx], ebo);
        }
        void VAO::addEBO(GLuint ebo) {
          assert(mNumOfVao == 1);
          addEBO(0, ebo);
        }
	GLuint
	VBO::getSize() const
	{
	    return mNumOfVbo;
	}
	void
	VBO::setData(GLuint vboIdx, const VerticesWithIndices &verticesWithIndices)
	{
	    assert(vboIdx < mNumOfVbo);

	    const auto indicesSize = verticesWithIndices.indices.size() * sizeof(GLint);
	    const auto verticesSize = verticesWithIndices.vertices.size() * sizeof(float);

	    // 开辟总空间（顶点+索引）
	    glCall(glNamedBufferStorage, vbo[vboIdx], indicesSize + verticesSize, nullptr, GL_DYNAMIC_STORAGE_BIT);
      

	    glCall(glNamedBufferSubData, vbo[vboIdx],verticesSize, indicesSize, verticesWithIndices.indices.data());//glNamedBufferSubData(buffer, ind_offset, ind_len, ind_data);

	    glCall(glNamedBufferSubData, vbo[vboIdx], 0, verticesSize, verticesWithIndices.vertices.data());//glNamedBufferSubData(buffer, 0, vrt_len, vrt_data);



	    withIndices[vboIdx] = true;
	    // 记录顶点buffer大小
	    verticesSizes[vboIdx] = verticesSize;
	}
	} // namespace glcore
}
