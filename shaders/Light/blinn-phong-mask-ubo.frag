#version 460 core
out vec4 FragColor;

in vec2 uv;
in vec3 normal;
in vec3 worldPosition;
uniform sampler2D baseColorTexture;
uniform sampler2D specularTexture;


uniform vec4 u_baseColorFactor;      // 基础颜色因子 (R,G,B,A)
uniform float u_specularFactor;      // 高光强度因子 (代替高光贴图)


layout(std140) uniform DirectionLight{
uniform vec3 ambient;
uniform vec3 lightColor;
uniform vec3 globalLightDirection;
uniform vec3 cameraPos;
uniform float spotIntensity;
};


void main()
{
	vec3 objectColor = texture(baseColorTexture, uv).rgb;

	vec3 normal = normalize(normal);
	vec3 lightDir = normalize(globalLightDirection);
	vec3 viewDir = normalize(worldPosition-cameraPos);

	float diffuse = clamp(dot(-lightDir, normal), 0.0, 1.0);
	vec3 diffuseColor = diffuse * lightColor * objectColor;

	float dotProduct = dot(-lightDir,normal);
	float flag = step(0.0, dotProduct);

	vec3 halfVector = normalize((-lightDir - viewDir)/2.0);

	float specular = pow(max(dot(halfVector, normal), 0.0), spotIntensity);

	float specularMask = texture(specularTexture,uv).r;

	vec3 specularColor = specular * lightColor * specularMask;
	
	vec3 ambient = ambient * objectColor;

	vec4 finalColor = vec4(ambient + diffuseColor + specularColor * flag, 1.0);
	//vec4 finalColor = vec4(objectColor, 1.0);

	FragColor = finalColor;
}
