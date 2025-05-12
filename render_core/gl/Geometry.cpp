#include "Geometry.hpp"
#include <boost/hana/fwd/front.hpp>
#include <vector>
namespace RGL {
namespace glcore {
Cube::Cube(float size) : Mesh(),positions(), uvs(), size(size), halfSize(size / 2.0f){
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
	0, 1, 2, 2, 3, 0,	 // Front face
	4, 5, 6, 6, 7, 4,	 // Back face
	8, 9, 10, 10, 11, 8,	 // Top face
	12, 13, 14, 14, 15, 12,	 // Bottom face
	16, 17, 18, 18, 19, 16,	 // Right face
	20, 21, 22, 22, 23, 20	 // Left face
    };
    normals = {
	// 前面
	0.0f,
	0.0f,
	1.0f,
	0.0f,
	0.0f,
	1.0f,
	0.0f,
	0.0f,
	1.0f,
	0.0f,
	0.0f,
	1.0f,
	// 后面
	0.0f,
	0.0f,
	-1.0f,
	0.0f,
	0.0f,
	-1.0f,
	0.0f,
	0.0f,
	-1.0f,
	0.0f,
	0.0f,
	-1.0f,

	// 上面
	0.0f,
	1.0f,
	0.0f,
	0.0f,
	1.0f,
	0.0f,
	0.0f,
	1.0f,
	0.0f,
	0.0f,
	1.0f,
	0.0f,

	// 下面
	0.0f,
	-1.0f,
	0.0f,
	0.0f,
	-1.0f,
	0.0f,
	0.0f,
	-1.0f,
	0.0f,
	0.0f,
	-1.0f,
	0.0f,

	// 右面
	1.0f,
	0.0f,
	0.0f,
	1.0f,
	0.0f,
	0.0f,
	1.0f,
	0.0f,
	0.0f,
	1.0f,
	0.0f,
	0.0f,

	// 左面
	-1.0f,
	0.0f,
	0.0f,
	-1.0f,
	0.0f,
	0.0f,
	-1.0f,
	0.0f,
	0.0f,
	-1.0f,
	0.0f,
	0.0f,
    };
    this->descs = FloatDescs {
	FloatDesc("inPos",3),
	FloatDesc("inUV", 2), FloatDesc("inNormal",3)};

    Mesh::vertLength = getVertexLength();

	Mesh::indicesCount = indices.size();
    const auto vertNumber = positions.size()/3;
    for (size_t i = 0; i < vertNumber; i++) {
	channeledVertices.push_back(positions[i * 3 + 0]);
	channeledVertices.push_back(positions[i * 3 + 1]);
	channeledVertices.push_back(positions[i * 3 + 2]);
	channeledVertices.push_back(uvs[i * 2 + 0]);
	channeledVertices.push_back(uvs[i * 2 + 1]);
	channeledVertices.push_back(normals[i * 3 + 0]);
	channeledVertices.push_back(normals[i * 3 + 1]);
	channeledVertices.push_back(normals[i * 3 + 2]);
    }

	this->indicesOffset = channeledVertices.size() * sizeof(decltype(channeledVertices[0]));

}


Sphere::Sphere(float radius) {
    // 声明纬线与经线的数量
    constexpr int numLatLines = 60;   // 纬线
    constexpr int numLongLines = 60;  // 经线

    // 2 通过两层循环（纬线在外，经线在内）->位置、uv
    for (int i = 0; i <= numLatLines; i++) {
	for (int j = 0; j <= numLongLines; j++) {
	    const float phi = i * glm::pi<float>() / numLatLines;
	    const float theta = j * 2 * glm::pi<float>() / numLongLines;

	    const float y = radius * cos(phi);
	    const float x = radius * sin(phi) * cos(theta);
	    const float z = radius * sin(phi) * sin(theta);

	    positions.push_back(x);
	    positions.push_back(y);
	    positions.push_back(z);

	    const float u = 1.0 - (float)j / (float)numLongLines;
	    const float v = 1.0 - (float)i / (float)numLatLines;

	    uvs.push_back(u);
	    uvs.push_back(v);

		normals.push_back(x);
	    normals.push_back(y);
	    normals.push_back(z);
 
	}
    }

	


    for (int i = 0; i < numLatLines; i++) {
	for (int j = 0; j < numLongLines; j++) {
	    int p1 = i * (numLongLines + 1) + j;
	    int p2 = p1 + numLongLines + 1;
	    int p3 = p1 + 1;
	    int p4 = p2 + 1;

	    indices.push_back(p1);
	    indices.push_back(p2);
	    indices.push_back(p3);

	    indices.push_back(p3);
	    indices.push_back(p2);
	    indices.push_back(p4);
	}
    }

    indicesCount = indices.size();	


	this->descs = FloatDescs{
	FloatDesc("inPos", 3),
	FloatDesc("inUV", 2),
	FloatDesc("inNormal", 3)
	};

    const auto vertLength = getVertexLength();
    const auto vertNumber = positions.size() / 3;
    for (size_t i = 0; i < vertNumber; i++) {
	channeledVertices.push_back(positions[i * 3 + 0]);
	channeledVertices.push_back(positions[i * 3 + 1]);
	channeledVertices.push_back(positions[i * 3 + 2]);
	channeledVertices.push_back(uvs[i * 2 + 0]);
	channeledVertices.push_back(uvs[i * 2 + 1]);
	channeledVertices.push_back(normals[i * 3 + 0]);
	channeledVertices.push_back(normals[i * 3 + 1]);
	channeledVertices.push_back(normals[i * 3 + 2]);

    }
    this->indicesOffset = channeledVertices.size() * sizeof(decltype(channeledVertices[0]));


}

Plane::Plane(float width, float height) {
    indicesCount = 6;
	float halfW = width / 2.0f;
	float halfH = height / 2.0f;

	std::vector<float> positions = {
	    	-halfW, -halfH, 0.0f,
		halfW, -halfH, 0.0f,
		halfW, halfH, 0.0f,
		-halfW, halfH, 0.0f,
	};
	std::vector<float> uvs = {
	    		0.0f, 0.0f,
		1.0f, 0.0f,
		1.0f, 1.0f,
		0.0f, 1.0f
	};

    std::vector<float> normals = {
        		0.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 1.0f,
    };

	indices = {
		0, 1, 2,
		2, 3, 0
	};
    this->descs = FloatDescs{
	FloatDesc("inPos", 3),
	FloatDesc("inUV", 2), FloatDesc("inNormal", 3)};

    const auto vertLength = getVertexLength();
    const auto vertNumber = positions.size()/3;
    for (size_t i = 0; i < vertNumber; i++) {
	channeledVertices.push_back(positions[i * 3 + 0]);
	channeledVertices.push_back(positions[i * 3 + 1]);
	channeledVertices.push_back(positions[i * 3 + 2]);
	channeledVertices.push_back(uvs[i * 2 + 0]);
	channeledVertices.push_back(uvs[i * 2 + 1]);
	channeledVertices.push_back(normals[i * 3 + 0]);
	channeledVertices.push_back(normals[i * 3 + 1]);
	channeledVertices.push_back(normals[i * 3 + 2]);
    }
    this->indicesOffset = channeledVertices.size() * sizeof(decltype(channeledVertices[0]));

}


}  // namespace glcore
}  // namespace RGL
