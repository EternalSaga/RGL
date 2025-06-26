#version 460 core
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inNormal;
// 新增：每实例的模型矩阵，从 location 3 开始
layout (location = 3) in mat4 aInstanceMatrix;

out vec2 uv;
out vec3 normal;
out vec3 worldPosition;


layout(std140) uniform CameraBlock{
    mat4 viewMatrix;
    mat4 projectionMatrix;
};


void main()
{
    // 计算世界坐标
    worldPosition = vec3(aInstanceMatrix * vec4(inPos*0.05, 1.0));

    // 计算最终的裁剪空间坐标
    
    gl_Position = projectionMatrix * viewMatrix * vec4(worldPosition, 1.0);

    normal = normalize(mat3(aInstanceMatrix) * inNormal);
    //normal = vec3(0.0, 1.0, 0.0);

    uv = inUV;
}