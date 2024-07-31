#version 460 core

in vec3 inPos;
in vec2 inUV;

out vec2 uv;

void main(){
gl_Position=vec4(inPos,1.0);
uv=inUV;
}
