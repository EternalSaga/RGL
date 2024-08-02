#version 460 core

out vec4 FragColor;

in vec2 uv;

uniform sampler2D sampler;//uniform 采样器

void main(){
	//使用sampler采样uv，再由texture函数生成颜色
	FragColor = texture(sampler,uv);
}
