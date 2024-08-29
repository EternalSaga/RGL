#include "DynamicEffect.hpp"
#include <SDL2/SDL_timer.h>
namespace RGL
{
namespace glcore
{
using namespace io;

const std::vector<float> pos_col_interleaved{-0.5f, -0.5f, 0.0f, 1.f, 0.f, 0.f,
    0.5f, -0.5f, 0.0f, 0.f, 1.f, 0.f,
    0.0f, 0.5f, 0.0f, 0.f, 0.f, 1.f};
const std::vector<GLint> eboSimple{0, 1, 2};

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
const std::vector<float> rectangle_pos_uv = {
    -0.5f,
    -0.5f,
    0.0f,
    0.0f,
    0.0f,
    0.5f,
    -0.5f,
    0.0f,
    1.3f,
    0.0f,
    -0.5f,
    0.5f,
    0.0f,
    0.0f,
    1.0f,
    0.5f,
    0.5f,
    0.0f,
    1.3f,
    1.0f,
};
const std::vector<GLint> rectangle_indeces = {0, 1, 2, 2, 1, 3};
ColorfulShiningTriangle::ColorfulShiningTriangle()
{
    vbo = std::make_unique<VBO>();
    vbo->setData(pos_col_interleaved);
    ebo = std::make_unique<EBO>();
    ebo->setData(eboSimple);
    vao = std::make_unique<VAO>();

    vao->addEBO(*ebo);

    ShaderSrcs shaders = {
	{SHADER_TYPE::VERTEX, {"shaders\\beginner\\UniformColor.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\UniformColor.frag"}}};
    shader = std::make_unique<Shader>(shaders);

    vao->setShaderProgram(*shader);
    vao->set(*vbo, 3, 6, 0, "inPos");
    vao->set(*vbo, 3, 6, 3, "inColor");
}
void
ColorfulShiningTriangle::operator()()
{
    shader->useProgram();

    shader->setUniform<float>("time", (((float)SDL_GetTicks64()) / 1000.0f));

    glCall(glBindVertexArray, *vao);
    glCall(glDrawElements, GL_TRIANGLES, 3, GL_UNSIGNED_INT, nullptr);
    glCall(glBindVertexArray, 0);
}
MovingTriangle::MovingTriangle()
{
    vbo = std::make_unique<VBO>();
    vbo->setData(pos_col_interleaved);
    ebo = std::make_unique<EBO>();
    ebo->setData(eboSimple);
    vao = std::make_unique<VAO>();

    vao->addEBO(*ebo);

    ShaderSrcs shaders = {
	{SHADER_TYPE::VERTEX, {"shaders\\beginner\\moving.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\moving.frag"}}};
    shader = std::make_unique<Shader>(shaders);

    vao->setShaderProgram(*shader);
    vao->set(*vbo, 3, 6, 0, "inPos");
    vao->set(*vbo, 3, 6, 3, "inColor");
}
void
MovingTriangle::operator()()
{
    shader->useProgram();
    shader->setUniform<float>("time", (((float)SDL_GetTicks64()) / 1000.0f));

    glCall(glBindVertexArray, *vao);
    glCall(glDrawElements, GL_TRIANGLES, 3, GL_UNSIGNED_INT, nullptr);
}
MovingTexture::MovingTexture()
{
    vbo = std::make_unique<VBO>();
    vbo->setData(rectangle_pos_uv);

    vao = std::make_unique<VAO>();
    ShaderSrcs shaders = {
	{SHADER_TYPE::VERTEX, {"shaders\\beginner\\movinguv.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\beginner\\movinguv.frag"}}};
    shader = std::make_unique<Shader>(shaders);

    texture = std::make_unique<Texture>();
    {
	io::LoadedImg img("./assest/001.jpg");
	texture->set(img, "001", false);
    }

    vao->setShaderProgram(*shader);
    auto desc = hana::make_tuple(VertexElement<float[3]>("inPos"),
	VertexElement<float[2]>("inUV"));
    vao->setDSA_interleaved(*vbo, desc);
    ebo = std::make_unique<EBO>();
    ebo->setData(rectangle_indeces);
    vao->addEBO(*ebo);
}
void
MovingTexture::operator()()
{
    shader->useProgram();

    shader->setUniform("sampler", texture->useTexture("001"));
    shader->setUniform<float>("timestamp", (((float)SDL_GetTicks64()) / 1000.0f));
    glCall(glBindVertexArray, *vao);
    glCall(glDrawElements, GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
}
BlendMipmap::BlendMipmap()
{
    vbo = std::make_unique<VBO>();
    vbo->setData(rectangle_pos_uv2);
    vao = std::make_unique<VAO>();
    ShaderSrcs shaders = {
	{SHADER_TYPE::VERTEX, {"shaders\\beginner\\uvshaderMipmap.vert"}},
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
    ebo = std::make_unique<EBO>();
    ebo->setData(rectangle_indeces);
    vao->addEBO(*ebo);
}
void
BlendMipmap::operator()()
{
    shader->useProgram();

     shader->setUniform("grass", grass_land_noise->useTexture("grass"));
    shader->setUniform("mudLand", grass_land_noise->useTexture("land"));
    shader->setUniform("randomNoise", grass_land_noise->useTexture("noise"));

    shader->setUniform<float>("time", (((float)SDL_GetTicks64()) / 1000.0f));

    glCall(glBindVertexArray, *vao);
    glCall(glDrawElements, GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
}
} // namespace glcore
} // namespace RGL
