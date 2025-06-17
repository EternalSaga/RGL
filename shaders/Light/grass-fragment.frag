#version 460 core
out vec4 FragColor;

in vec2 uv;
in vec3 normal;         // 从顶点着色器传入的世界空间法线
in vec3 worldPosition;

uniform sampler2D baseColorTexture;
uniform sampler2D specularTexture;

// --- 光照 Uniforms ---
layout(std140) uniform DirectionLight{
    uniform vec3 ambient;
    uniform vec3 lightColor;
    uniform vec3 globalLightDirection; // 假设这是光线行进的方向, e.g., from sun to origin
    uniform vec3 cameraPos;
    uniform float spotIntensity;
};

// --- 材质 Uniforms ---
layout(std140) uniform pbrUniformBlock{
	vec4 baseColor;
	float metallicFactor;
	float roughnessFactor;
	int u_hasBaseColorTexture;
	int u_hasSpecularTexture;
};

// 新增一个控制半透明效果强度的Uniform
uniform float u_translucency = 0.8; // 值越大，透光效果越强

void main()
{
	// 1. --- 获取基础颜色和Alpha ---
	vec4 albedo = texture(baseColorTexture, uv);
    // 在你的PBR逻辑中，这里可以被覆盖
	// if(bool(u_hasBaseColorTexture)) { albedo = baseColor; }
    
    // Alpha 测试 (更适合Alpha混合，但我们暂时保留discard)
	if (albedo.a < 0.1) {
		discard;
	}

	// 2. --- 准备光照向量 ---
    // 标准化法线，并根据面朝向决定使用正面还是背面法线
	vec3 N = normalize(normal);
    if (!gl_FrontFacing) {
        N = -N; // 如果是背面，翻转法线
    }
    
    // 视图方向：从片元指向摄像机 (已修正)
	vec3 viewDir = normalize(cameraPos - worldPosition);
    // 光线方向：从片元指向光源 (我们假设globalLightDirection是光线的传播方向)
	vec3 lightDir = normalize(-globalLightDirection);

	// 3. --- 计算标准漫反射 (Diffuse) ---
	float NdotL = max(dot(N, lightDir), 0.0);
	vec3 diffuse = NdotL * lightColor * albedo.rgb;

	// 4. --- 计算高光 (Specular) ---
	vec3 halfwayDir = normalize(lightDir + viewDir);
	float spec = pow(max(dot(N, halfwayDir), 0.0), spotIntensity);
	float specularMask = texture(specularTexture, uv).r; // 假设高光遮罩在R通道
	vec3 specular = spec * lightColor * specularMask;

    // 5. --- (核心改进) 计算半透明/次表面散射 (Translucency / SSS) ---
    // 这个效果模拟了光线从草叶背面透射过来的效果
    vec3 backLightDir = -lightDir;
    float transDot = max(dot(N, backLightDir), 0.0);
    vec3 translucency = pow(transDot, 2.0) * lightColor * albedo.rgb * u_translucency;

	// 6. --- 组合最终颜色 ---
    // 最终颜色 = 环境光 + 漫反射 + 高光 + 透射光
	vec3 finalColor = (ambient * albedo.rgb) + diffuse + specular + translucency;

	FragColor = vec4(finalColor, albedo.a);
}