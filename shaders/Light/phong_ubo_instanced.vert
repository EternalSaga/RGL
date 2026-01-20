#version 460 core

struct Vertex {
    vec3 inPos;
    vec2 inUV;
    vec3 inNormal;
};

layout(std430, binding = 2) readonly buffer VertexBuffer {
    Vertex vertices[];
};

layout(location = 0) out vec2 uv;

layout(location = 1) out vec3 normal;
layout(location = 2) out vec3 worldPosition;


layout(std140,binding = 0) uniform CameraBlock{
    mat4 viewMatrix;
    mat4 projectionMatrix;
};


layout(std430,binding = 1) buffer InstanceData{
    mat4 instanceMatrices[];
};


void main()
{
    Vertex v = vertices[gl_VertexIndex];
    mat4 instanceMatrix = instanceMatrices[gl_InstanceIndex];

    // 计算世界坐标
    worldPosition = vec3(instanceMatrix * vec4(v.inPos, 1.0));

    // 计算最终的裁剪空间坐标
    
    gl_Position = projectionMatrix * viewMatrix * vec4(worldPosition, 1.0);

    normal = normalize(mat3(instanceMatrix) * v.inNormal);


    uv = v.inUV;
}