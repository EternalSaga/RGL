#version 460 core
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
//自定义一个传向管线下一阶段的变量
out vec3 color;

void main(){
 gl_Position = vec4(inPos.x, inPos.y, inPos.z, 1.0);
 color = inColor;
}
