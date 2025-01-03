#version 460 core
out vec4 FragColor;

in vec2 uv;
in vec3 normal;
in vec3 worldPosition;//顶点经过插值后产生的fragment的世界坐标

uniform sampler2D sampler;

layout(std140) uniform SpotLight{
vec3 ambient;
vec3 lightColor;
vec3 targetDirection;
vec3 lightPosition;
vec3 cameraPos;
float spotIntensity;
float innerAngle;//弧度
float outerAngle;//弧度

};




void main()
{
	vec3 objectColor = texture(sampler, uv).rgb; 
	vec3 normal = normalize(normal);
	vec3 lightDir = normalize(worldPosition - lightPosition); // 光线方向
	vec3 viewDir = normalize(worldPosition-cameraPos); // 视线方向

    vec3 targetDirectionNormal = normalize(targetDirection);

    float cosTheta = dot(lightDir, targetDirectionNormal); // Cosine of the angle between light direction and target direction
    float cosInner = cos(innerAngle);
    float cosOuter = cos(outerAngle);

    // Smooth step for smoother attenuation
    float spotAttenuation = smoothstep(cosOuter, cosInner, cosTheta);

	//计算漫反射
	float diffuse = clamp(dot(-lightDir, normal), 0.0, 1.0);
	vec3 diffuseColor = diffuse * lightColor * objectColor;

	float dotProduct = dot(-lightDir,normal);
	float flag = step(0.0, dotProduct);

	vec3 halfVector = normalize((-lightDir - viewDir)/2.0);

	float specular = pow(max(dot(halfVector, normal), 0.0), spotIntensity);

	vec3 specularColor = specular * lightColor * flag;
	
	vec3 ambient = ambient * objectColor;

	vec4 finalColor = vec4(ambient + (diffuseColor + specularColor )*spotAttenuation, 1.0);

	FragColor = finalColor;
}
