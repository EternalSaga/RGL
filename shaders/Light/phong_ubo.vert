#version 460 core

in vec3 inPos;
in vec2 inUV;
in vec3 inNormal;


out vec2 uv;
out vec3 normal;
out vec3 worldPosition;

layout(std140) uniform Transforms{
	mat4 modelMatrix;
	mat4 MVP;
	mat3 inverseModelMatrix;
};

void main()
{
	//输入顶点位置转换为齐次坐标
	vec4 transformPosition = vec4(inPos, 1.0);
	//计算当前顶点的worldPosition，并且向后传输给FragmentShader
	transformPosition = Transforms.modelMatrix * transformPosition;
	worldPosition = transformPosition.xyz;

	//将顶点位置从模型空间转换到裁剪空间
	gl_Position = Transforms.MVP *  vec4(inPos, 1.0);
	
	uv = inUV;
	//在CPU端完成了modelmatrix的逆矩阵和转置计算，这里直接使用即可
	normal = Transforms.inverseModelMatrix * inNormal;
}
