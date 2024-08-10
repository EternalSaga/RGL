#version 460 core
out vec4 FragColor;
in vec3 color;
uniform float time;
void main(){
	float intensity = (sin(time) + 1.0)/2.0;
	FragColor = vec4(vec3(intensity)  + color, 1.0f);
}
