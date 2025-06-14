#version 460 core
out vec4 FragColor;

in vec2 uv;
in vec3 normal;
in vec3 worldPosition;
uniform sampler2D baseColorTexture;
uniform sampler2D specularTexture;


layout(std140) uniform DirectionLight{
uniform vec3 ambient;
uniform vec3 lightColor;
uniform vec3 globalLightDirection;
uniform vec3 cameraPos;
uniform float spotIntensity;
};

layout(std140) uniform pbrUniformBlock{
	
	vec4 baseColor;
	float metallicFactor;
	float roughnessFactor;
	int u_hasBaseColorTexture;
	int u_hasSpecularTexture;
};



void main()
{
	vec3 objectColor;
	if(bool(u_hasBaseColorTexture)){
		objectColor = baseColor.rgb;
	}else{
		objectColor = texture(baseColorTexture, uv).rgb;
		
	}

	float alpha = 1.0;

	if(bool(u_hasBaseColorTexture)){
		alpha = baseColor.a;
	}

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

	vec4 finalColor = vec4(ambient + diffuseColor + specularColor * flag, alpha);


	FragColor = finalColor;
}
