#version 460 core
in vec3 inPos;
in vec2 inUV;


out vec2 uv;

uniform mat4 transform;
uniform mat4 viewMatrix;
uniform mat4 projectionMatrix;

//inPos作为attribute（属性）传入shader
//不允许更改的
void main()
{
	vec4 position = vec4(inPos, 1.0);
	position = projectionMatrix * viewMatrix * transform * position;
	gl_Position = position;

	uv = inUV;
}