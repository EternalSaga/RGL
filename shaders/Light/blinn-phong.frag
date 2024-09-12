#version 460 core
out vec4 FragColor;

in vec2 uv;
in vec3 normal;
uniform sampler2D sampler;
uniform vec3 ambient;

uniform vec3 lightColor;
uniform vec3 globalLightDirection;

uniform vec3 cameraPos;
in vec3 worldPosition;;
void main()
{
	vec3 objectColor = texture(sampler, uv).rgb;

	vec3 normal = normalize(normal);
	vec3 lightDir = normalize(globalLightDirection);
	vec3 viewDir = normalize(worldPosition-cameraPos);

	float diffuse = clamp(dot(-lightDir, normal), 0.0, 1.0);
	vec3 diffuseColor = diffuse * lightColor * objectColor;

	float dotProduct = dot(-lightDir,normal);
	float flag = step(0.0, dotProduct);

	vec3 halfVector = normalize((-lightDir - viewDir)/2.0);

	float specular = pow(max(dot(halfVector, normal), 0.0), 32.0);
	vec3 specularColor = specular * lightColor;
	
	vec3 ambient = ambient * objectColor;


	vec4 finalColor = vec4(ambient + diffuseColor + specularColor * flag, 1.0);

	FragColor = finalColor;
}