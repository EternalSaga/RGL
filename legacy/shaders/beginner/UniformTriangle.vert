#version 460 core
in vec3 inPos;
void main(){

	gl_Position = vec4(vec3(inPos) , 1.0f);
}
