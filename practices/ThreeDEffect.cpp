#include "ThreeDEffect.hpp"
#include "GLObj.hpp"
#include "GLTextures.hpp"
#include "Shader.hpp"

namespace RGL
{
namespace practice
{
using namespace glcore;
const std::vector<float> rectangle_pos_uv2 = {
    -0.5f,
    -0.5f,
    0.0f,
    0.0f,
    0.0f,
    0.5f,
    -0.5f,
    0.0f,
    1.0f,
    0.0f,
    -0.5f,
    0.5f,
    0.0f,
    0.0f,
    1.0f,
    0.5f,
    0.5f,
    0.0f,
    1.0f,
    1.0f,
};
const std::vector<GLint> rectangle_indeces = {0, 1, 2, 2, 1, 3};

BasicTransform::BasicTransform() : trans(1.0f)
{

    VerticesWithIndices vi{rectangle_pos_uv2, rectangle_indeces};

    vbo = std::make_unique<VBO>();
    vbo->setData(0, vi);
    vao = std::make_unique<VAO>();
    ShaderSrcs shaders = {
	{SHADER_TYPE::VERTEX, {"shaders\\beginner\\BasicTransform.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\Blend.frag"}}};
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
    auto desc = hana::make_tuple(VertexElement<float[3]>("inPos"),
	VertexElement<float[2]>("inUV"));
    vao->setDSA_interleaved(*vbo, desc);
    // 因为vbo中已经包含了顶点索引，所以这里不需要额外创建ebo，直接把vbo添加到vao的ebo属性即可
    vao->addEBO(*vbo);
    // 阿西吧，即使是DSA，在draw之前还是得绑定下vao，让draw知道到底画谁
    glCall(glBindVertexArray, *vao);
}
void
BasicTransform::operator()()
{
    shader->useProgram();
    shader->setUniform("grass", grass_land_noise->getTextureUnitID(0));
    shader->setUniform("mudLand", grass_land_noise->getTextureUnitID(1));
    shader->setUniform("randomNoise", grass_land_noise->getTextureUnitID(2));
    doTransform();

    shader->setUniformMat(
	"transformMat",
	trans);
    // 注意保证host和device端的类型一致，别把shader类型写成vec4
    // 因为ebo存在于vbo中，所以最后一个参数要指定偏移量，这里的ebo位于vbo末尾，所以偏移量是vbo里顶点属性的大小
    glCall(glDrawElements, GL_TRIANGLES, rectangle_indeces.size(), GL_UNSIGNED_INT, reinterpret_cast<void *>(vbo->getVerticesSize(0)));
}
CameraTransform::CameraTransform(std::shared_ptr<Camera> cam)
{
    this->cam = cam;
    VerticesWithIndices vi{rectangle_pos_uv2, rectangle_indeces};

    vbo = std::make_unique<VBO>();
    vbo->setData(0, vi);
    vao = std::make_unique<VAO>();
    ShaderSrcs shaders = {
	{SHADER_TYPE::VERTEX, {"shaders\\beginner\\ControllerTransform.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\ControllerTransform.frag"}}};

    this->shader = std::make_unique<Shader>(shaders);
    doraemon = std::make_unique<Texture>();
    {
	io::LoadedImg doraemonImg("./assest/doraemon.jpg");
	doraemon->set(doraemonImg, 0, true);
    }
    vao->setShaderProgram(*shader);
    auto desc = hana::make_tuple(VertexElement<float[3]>("inPos"),
	VertexElement<float[2]>("inUV"));
    vao->setDSA_interleaved(*vbo, desc);
    // 因为vbo中已经包含了顶点索引，所以这里不需要额外创建ebo，直接把vbo添加到vao的ebo属性即可
    vao->addEBO(*vbo);
    // 阿西吧，即使是DSA，在draw之前还是得绑定下vao，让draw知道到底画谁
    glCall(glBindVertexArray, *vao);
}
void CameraTransform::operator()(){
    shader->useProgram();
    shader->setUniform("sampler", doraemon->getTextureUnitID(0));
    shader->setUniformMat("transform", this->transform);
    shader->setUniformMat("viewMatrix", cam->getViewMatrix());
    shader->setUniformMat("projectionMatrix", cam->getProjectionMatrix());
    glCall(glDrawElements, GL_TRIANGLES, rectangle_indeces.size(), GL_UNSIGNED_INT, reinterpret_cast<void *>(vbo->getVerticesSize(0)));
}
} // namespace practice
} // namespace RGL
