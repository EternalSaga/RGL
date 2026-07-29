#version 460 core
out vec4 FragColor;

in vec2 uv;
in vec3 normal;
uniform sampler2D sampler;

uniform vec3 ambient;

uniform vec3 lightColor;


uniform float innerAngle;
uniform float outerAngle;
uniform vec3 targetDirection;
uniform float spotIntensity;
uniform vec3 lightPosition;

uniform vec3 cameraPos;
in vec3 worldPosition;//顶点经过插值后产生的fragment的世界坐标

void main()
{
	vec3 objectColor = texture(sampler, uv).rgb; 
	vec3 normal = normalize(normal);
	vec3 lightDir = normalize(worldPosition - lightPosition); // 光线方向
	vec3 viewDir = normalize(worldPosition-cameraPos); // 视线方向

    vec3 targetDirectionNormal = normalize(targetDirection);

    float cGamma = dot(lightDir, targetDirectionNormal);
    float spotAttenuation =clamp( (cGamma - outerAngle) / (outerAngle-innerAngle), 0.0, 1.0);

	//计算漫反射
	float diffuse = clamp(dot(-lightDir, normal), 0.0, 1.0);
	vec3 diffuseColor = diffuse * lightColor * objectColor;

	float dotProduct = dot(-lightDir,normal);
	float flag = step(0.0, dotProduct);

	vec3 halfVector = normalize((-lightDir - viewDir)/2.0);

	float specular = pow(max(dot(halfVector, normal), 0.0), spotIntensity);
    //暂时先去掉蒙板，又不是所有物体都需要蒙板
	//float specularMask = texture(spMask,uv).r;
	//vec3 specularColor = specular * lightColor * specularMask * flag;
	vec3 specularColor = specular * lightColor * flag;
	
	vec3 ambient = ambient * objectColor;

	vec4 finalColor = vec4(ambient + (diffuseColor + specularColor )*spotAttenuation, 1.0);
	//vec4 finalColor = vec4(ambient, 1.0);

	FragColor = finalColor;
}
