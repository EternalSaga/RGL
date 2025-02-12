#version 460 core
out vec4 FragColor;
uniform sampler2D baseColorTexture;
void main()
{
	vec4 texColor = texture(baseColorTexture, gl_FragCoord.xy); // Use the fragment's screen coordinates to sample the texture.

	FragColor = texColor;
}
