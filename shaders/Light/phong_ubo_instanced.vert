#version 460 core
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inNormal;


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
    mat4 instanceMatrix = instanceMatrices[gl_InstanceIndex];

    // 计算世界坐标
    worldPosition = vec3(instanceMatrix * vec4(inPos, 1.0));

    // 计算最终的裁剪空间坐标
    
    gl_Position = projectionMatrix * viewMatrix * vec4(worldPosition, 1.0);

    normal = normalize(mat3(instanceMatrix) * inNormal);


    uv = inUV;
}