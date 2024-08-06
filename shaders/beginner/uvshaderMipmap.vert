#version 460 core

in vec3 inPos;
in vec2 inUV;

out vec2 uv;
uniform float time;
void main(){

float scale = 1.0 / time;
 vec3 scaledPos = inPos * scale;

gl_Position=vec4(scaledPos,1.0);
uv=inUV;
}
