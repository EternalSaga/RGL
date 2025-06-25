#version 460 core
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inNormal;
// 新增：每实例的模型矩阵，从 location 3 开始
layout (location = 3) in mat4 aInstanceMatrix;

out vec2 uv;
out vec3 normal;
out vec3 worldPosition;

// Transforms UBO 依然有用，但 modelMatrix 现在来自实例数据
layout(std140) uniform Transforms{
	mat4 modelMatrix; // 这个现在可以作为“草地组”的整体变换，或者干脆不用
	mat4 MVP;         // 这个不再需要从CPU计算传入，我们在shader里算
	mat4 inverseModelMatrix; // 这个也需要重新计算
};

// 新增：视图和投影矩阵的UBO，或者直接用 uniform
// 推荐单独创建一个 Camera UBO
uniform mat4 viewMatrix;
uniform mat4 projectionMatrix;


void main()
{
    // 计算世界坐标
    worldPosition = vec3(aInstanceMatrix * vec4(inPos, 1.0));

    // 计算最终的裁剪空间坐标
    gl_Position = projectionMatrix * viewMatrix * vec4(worldPosition, 1.0);

    // 变换法线到世界空间
    // 注意：我们用 aInstanceMatrix 的逆转置矩阵来变换法线
    normal = mat3(transpose(inverse(aInstanceMatrix))) * inNormal;

    uv = inUV;
}