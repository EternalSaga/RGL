#version 460 core

in vec2 uv;

out vec4 FragColor;

uniform sampler2D grass;
uniform sampler2D mudLand;
uniform sampler2D randomNoise;

void main(){
	vec4 grassColor = texture(grass, uv);
	vec4 landColor = texture(mudLand, uv);
	vec4 noiseColor = texture(randomNoise, uv);
	float weight = noiseColor.r;
	//	vec4 finalColor = grassColor * (1.0 - weight) + landColor * weight;
	vec4 finalColor = mix(grassColor, landColor,weight);//weight是land权重，grass就是1-weight权重

	FragColor = vec4(finalColor.rgb, 1.0);
}
