#version 460 core
in vec3 inPos;
in vec3 inColor;
//自定义一个传向管线下一阶段的变量
out vec3 color;

uniform float time;
void main(){
 gl_Position = vec4(inPos.x, inPos.y, inPos.z, 1.0);
 color = inColor * (sin(time) + 1.0) / 2.0;
}
