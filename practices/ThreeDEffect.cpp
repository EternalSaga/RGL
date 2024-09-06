#include "ThreeDEffect.hpp"
#include "GLObj.hpp"
#include "GLTextures.hpp"
#include "Shader.hpp"
#include "ProgrammedTexture.hpp"
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
	grass_land_noise->set(grassImg, "grass", true);
    }
    {
	io::LoadedImg landImg("./assest/land.jpg");
	grass_land_noise->set(landImg, "land", true);
    }
    {
	io::LoadedImg noiseImg("./assest/noise.jpg");
	grass_land_noise->set(noiseImg, "noise", true);
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
    shader->setUniform("grass", grass_land_noise->useTexture("grass"));
    shader->setUniform("mudLand", grass_land_noise->useTexture("land"));
    shader->setUniform("randomNoise", grass_land_noise->useTexture("noise"));
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
	doraemon->set(doraemonImg, "doraemon", true);
    }
    vao->setShaderProgram(*shader);
    auto desc = hana::make_tuple(VertexElement<float[3]>("inPos"),
	VertexElement<float[2]>("inUV"));
    vao->setDSA_interleaved(*vbo, desc);
    vao->addEBO(*vbo);
    glCall(glBindVertexArray, *vao);
}
void CameraTransform::operator()(){
    shader->useProgram();
    shader->setUniform("sampler", doraemon->useTexture("doraemon"));
    shader->setUniformMat("transform", this->transform);
    shader->setUniformMat("viewMatrix", cam->getViewMatrix());
    shader->setUniformMat("projectionMatrix", cam->getProjectionMatrix());
    glCall(glDrawElements, GL_TRIANGLES, rectangle_indeces.size(), GL_UNSIGNED_INT, reinterpret_cast<void *>(vbo->getVerticesSize(0)));
}

DepthTestExecise::DepthTestExecise(std::shared_ptr<Camera> cam){
    this->cam = cam;
    VerticesWithIndices vi{rectangle_pos_uv2, rectangle_indeces};
    vbo = std::make_unique<VBO>();
    vbo->setData(0, vi);
    vao = std::make_unique<VAO>(2);//有两个vao，一个用于描述哆啦A梦，一个用于描述野比大雄
    ShaderSrcs shaders = {
	{SHADER_TYPE::VERTEX, {"shaders\\beginner\\ControllerTransform.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\ControllerTransform.frag"}}};

    this->shader = std::make_unique<Shader>(shaders);
    doraemon_nobita = std::make_unique<Texture>(2);
    {
	io::LoadedImg doraemonImg("./assest/doraemon.jpg");
	doraemon_nobita->set(doraemonImg, "doraemon", true);
	io::LoadedImg nobita_nobiImg("./assest/nobita_nobi.jpg");
	doraemon_nobita->set(nobita_nobiImg, "nobita_nobi", true);
    }
    vao->setShaderProgram(*shader);
    auto desc = hana::make_tuple(VertexElement<float[3]>("inPos"),
	VertexElement<float[2]>("inUV"));
    vao->setDSA_interleaved(0, *vbo, desc);
    vao->setDSA_interleaved(1, *vbo, desc);
    vao->addEBO(0, *vbo);
    vao->addEBO(1, *vbo);
}

void DepthTestExecise::operator()(){
    shader->useProgram();

	glCall(glBindVertexArray,(*vao)[0]);
    shader->setUniform("sampler", doraemon_nobita->useTexture("doraemon"));
    shader->setUniformMat("transform", this->transform);
    shader->setUniformMat("viewMatrix", cam->getViewMatrix());
    shader->setUniformMat("projectionMatrix", cam->getProjectionMatrix());
	glCall(glDrawElements, GL_TRIANGLES, rectangle_indeces.size(), GL_UNSIGNED_INT, reinterpret_cast<void *>(vbo->getVerticesSize(0)));
    shader->setUniformMat("transform", glm::translate(this->transform,glm::vec3(0.8f,0.0f,-1.0f)));
	glCall(glBindVertexArray, (*vao)[1]);
    shader->setUniform("sampler", doraemon_nobita->useTexture("nobita_nobi"));
	glCall(glDrawElements, GL_TRIANGLES, rectangle_indeces.size(), GL_UNSIGNED_INT, reinterpret_cast<void *>(vbo->getVerticesSize(0)));
}

DrawCube::DrawCube(std::shared_ptr<Camera> cam)
{
    this->cam = cam;
    ShaderSrcs shaders = {
	{SHADER_TYPE::VERTEX, {"shaders\\beginner\\ControllerTransform.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\ControllerTransform.frag"}}};
    this->shader = std::make_unique<Shader>(shaders);
    this->shader = std::make_unique<Shader>(shaders);
    checkboarder = std::make_unique<Texture>();
    {
	texture::CheckerBoard cb(8,8);
	checkboarder->set(cb.getTexture(), "checkboarder", true);
    checkboarder->setFilltering("checkboarder", GL_NEAREST);
    }
    std::unique_ptr<CommonGeometry> geometry = std::make_unique<Cube>(6.0f,*shader);
	this->vao = std::move(geometry->getVAO());
    auto [ vertCount, idxOffset ] = geometry->getIdicesCountAndOffset();
	mvertCount = vertCount;
    midxOffset = idxOffset;
}
void
DrawCube::operator()()
{
    glCall(glBindVertexArray, *vao);
    shader->useProgram();
    shader->setUniform("sampler", checkboarder->useTexture("checkboarder"));
    shader->setUniformMat("transform", this->transform);
    shader->setUniformMat("viewMatrix", cam->getViewMatrix());
    shader->setUniformMat("projectionMatrix", cam->getProjectionMatrix());

    shader->setUniform("cameraPos", cam->position.x, cam->position.y, cam->position.z);

    glCall(glDrawElements, GL_TRIANGLES, mvertCount, GL_UNSIGNED_INT, reinterpret_cast<void *>(midxOffset));
}
} // namespace practice
} // namespace RGL
