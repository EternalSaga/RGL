#version 460 core
out vec4 FragColor;
uniform vec3 ucolor;

void main(){
	
	FragColor = vec4(vec3(ucolor), 1.0f);
}
