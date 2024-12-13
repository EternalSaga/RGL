#version 460 core
out vec4 FragColor;

in vec2 uv;
in vec3 normal;
uniform sampler2D sampler;
uniform sampler2D spMask;
uniform vec3 ambient;

uniform vec3 lightColor;
uniform vec3 lightPosition;

uniform float k2;
uniform float k1;
uniform float kc;
uniform float spotIntensity;//shininess

uniform vec3 cameraPos;
in vec3 worldPosition;//顶点经过插值后产生的fragment的世界坐标

void main()
{
	vec3 objectColor = texture(sampler, uv).rgb; 
	vec3 normal = normalize(normal);
	vec3 lightDir = normalize(worldPosition - lightPosition); // 光线方向
	vec3 viewDir = normalize(worldPosition-cameraPos); // 视线方向

	//计算衰减
	float dist = length(lightPosition - worldPosition);
	float attenuation = 1.0 / (kc + k1 * dist + k2 * pow(dist, 2.0));

	//计算漫反射
	float diffuse = clamp(dot(-lightDir, normal), 0.0, 1.0);
	vec3 diffuseColor = diffuse * lightColor * objectColor;

	float dotProduct = dot(-lightDir,normal);
	float flag = step(0.0, dotProduct);

	vec3 halfVector = normalize((-lightDir - viewDir)/2.0);

	float specular = pow(max(dot(halfVector, normal), 0.0), spotIntensity);

	float specularMask = texture(spMask,uv).r;

	vec3 specularColor = specular * lightColor * specularMask * flag;
	
	vec3 ambient = ambient * objectColor;

	vec4 finalColor = vec4(ambient + (diffuseColor + specularColor )*attenuation, 1.0);

	FragColor = finalColor;
}
