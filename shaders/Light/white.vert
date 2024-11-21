#version 460 core

in vec3 inPos;

uniform mat4 modelMatrix;
uniform mat4 viewMatrix;
uniform mat4 projectionMatrix;


void main()
{
	//输入顶点位置转换为齐次坐标
	vec4 transformPosition = vec4(inPos, 1.0);


	//将顶点位置从模型空间转换到裁剪空间
	gl_Position = projectionMatrix * viewMatrix * transformPosition;
	
}
