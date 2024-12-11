#version 460 core

in vec3 inPos;

uniform mat4 MVP;


void main()
{
	//将顶点位置从模型空间转换到裁剪空间
	gl_Position = MVP *  vec4(inPos, 1.0);
	
}
