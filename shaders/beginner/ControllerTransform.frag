#version 460 core
out vec4 FragColor;

in vec2 uv;

uniform sampler2D sampler;

void main()
{
  FragColor = texture(sampler, uv);
}