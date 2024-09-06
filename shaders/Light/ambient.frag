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

	vec3 lightReflect = normalize(reflect(lightDir, normal));

	float specular = max(dot(lightReflect,-viewDir), 0.0);
	specular = pow(specular, 32.0);
	vec3 specularColor = specular * lightColor;

	FragColor = vec4(ambient*1.5 + diffuseColor + specularColor * flag, 1.0);
}