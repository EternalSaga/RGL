#version 460 core

in vec3 inPos;
in vec2 inUV;

out vec2 uv;

uniform float  timestamp;

void main(){
gl_Position=vec4(inPos,1.0);

float deltaU = timestamp*0.3;

uv=vec2(inUV.x + deltaU,inUV.y);
}
