#version 460 core

in vec3 inPos;
in vec2 inUV;
in vec3 inNormal;


out vec2 uv;
out vec3 normal;
out vec3 worldPosition;

uniform mat4 modelMatrix;
uniform mat4 viewMatrix;
uniform mat4 projectionMatrix;


void main()
{
	//输入顶点位置转换为齐次坐标
	vec4 transformPosition = vec4(inPos, 1.0);
	//计算当前顶点的worldPosition，并且向后传输给FragmentShader
	transformPosition = modelMatrix * transformPosition;
	worldPosition = transformPosition.xyz;

	//将顶点位置从模型空间转换到裁剪空间
	gl_Position = projectionMatrix * viewMatrix * transformPosition;
	
	uv = inUV;
	//求逆变换最好在CPU端完成
	normal = transpose(inverse(mat3(modelMatrix))) * inNormal;
}
