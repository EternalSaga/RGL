#version 460 core
in vec3 inPos;
in vec3 inColor;
//自定义一个传向管线下一阶段的变量
out vec3 color;

uniform float time;
void main(){
float dx = 0.3;
float offsetX = sin(time) * dx;
 gl_Position = vec4(inPos.x + offsetX, inPos.y, inPos.z, 1.0);
 color = inColor;
}
