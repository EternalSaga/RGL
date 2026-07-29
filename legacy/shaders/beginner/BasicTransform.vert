#version 460 core

in vec3 inPos;
in vec2 inUV;

out vec2 uv;
uniform mat4 transformMat;
void main(){

	vec4 pos4 = vec4(inPos,1.0);


	gl_Position = pos4 * transformMat;
	uv=inUV;
}
