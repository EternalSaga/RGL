#include "Geometry.hpp"
namespace RGL
{
namespace glcore
{
Cube::Cube(float size, GLuint shaderProgram) : size(size), halfSize(size / 2.0f)
{

    indicesCount = 36;
    positions = {
	// Front face
	-halfSize, -halfSize, halfSize, halfSize, -halfSize, halfSize, halfSize, halfSize, halfSize, -halfSize, halfSize, halfSize,
	// Back face
	-halfSize, -halfSize, -halfSize, -halfSize, halfSize, -halfSize, halfSize, halfSize, -halfSize, halfSize, -halfSize, -halfSize,
	// Top face
	-halfSize, halfSize, halfSize, halfSize, halfSize, halfSize, halfSize, halfSize, -halfSize, -halfSize, halfSize, -halfSize,
	// Bottom face
	-halfSize, -halfSize, -halfSize, halfSize, -halfSize, -halfSize, halfSize, -halfSize, halfSize, -halfSize, -halfSize, halfSize,
	// Right face
	halfSize, -halfSize, halfSize, halfSize, -halfSize, -halfSize, halfSize, halfSize, -halfSize, halfSize, halfSize, halfSize,
	// Left face
	-halfSize, -halfSize, -halfSize, -halfSize, -halfSize, halfSize, -halfSize, halfSize, halfSize, -halfSize, halfSize, -halfSize};
    uvs = {
	0.0,
	0.0,
	1.0,
	0.0,
	1.0,
	1.0,
	0.0,
	1.0,
	0.0,
	0.0,
	1.0,
	0.0,
	1.0,
	1.0,
	0.0,
	1.0,
	0.0,
	0.0,
	1.0,
	0.0,
	1.0,
	1.0,
	0.0,
	1.0,
	0.0,
	0.0,
	1.0,
	0.0,
	1.0,
	1.0,
	0.0,
	1.0,
	0.0,
	0.0,
	1.0,
	0.0,
	1.0,
	1.0,
	0.0,
	1.0,
	0.0,
	0.0,
	1.0,
	0.0,
	1.0,
	1.0,
	0.0,
	1.0,
    };
    indices = {
	0, 1, 2, 2, 3, 0,	// Front face
	4, 5, 6, 6, 7, 4,	// Back face
	8, 9, 10, 10, 11, 8,	// Top face
	12, 13, 14, 14, 15, 12, // Bottom face
	16, 17, 18, 18, 19, 16, // Right face
	20, 21, 22, 22, 23, 20	// Left face
    };

    const auto desc = hana::make_tuple(VertexElement<float[3]>("inPos"),
	VertexElement<float[2]>("inUV"));

    for (size_t i = 0; i < positions.size() / 3; i++) {
	position_uvs.push_back(positions[i * 3 + 0]);
	position_uvs.push_back(positions[i * 3 + 1]);
	position_uvs.push_back(positions[i * 3 + 2]);
	position_uvs.push_back(uvs[i * 2 + 0]);
	position_uvs.push_back(uvs[i * 2 + 1]);

    }
    this->vao = std::make_unique<VAO>();
    this->vbo = std::make_unique<VBO>();

    vbo->setData(0, {position_uvs, indices});
    vao->setShaderProgram(shaderProgram);
    vao->setDSA_interleaved(*vbo, desc);
    vao->addEBO(*vbo);
}
std::tuple<size_t, size_t>
CommonGeometry::getIdicesCountAndOffset()
{
    return std::tuple<size_t, size_t>{indicesCount, vbo->getVerticesSize()};
}
std::unique_ptr<VAO>
CommonGeometry::getVAO()
{
    return std::move(vao);
}

} // namespace glcore
} // namespace RGL
