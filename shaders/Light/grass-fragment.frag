#version 460 core
out vec4 FragColor;

in vec2 uv;
in vec3 normal;
in vec3 worldPosition;

uniform sampler2D baseColorTexture;
// specularTexture 在这个草地光照模型中可以不用，因为草基本没有镜面反射
// uniform sampler2D specularTexture;

// --- Uniforms ---
layout(std140) uniform DirectionLight{
    uniform vec3 ambient;
    uniform vec3 lightColor;
    uniform vec3 globalLightDirection;
    uniform vec3 cameraPos;
    uniform float spotIntensity; // 这个对于草地来说意义不大，可以忽略
};

uniform float u_translucency = 0.5; // 透光效果强度

void main()
{
	vec4 albedo = texture(baseColorTexture, uv);
    
	if (albedo.a < 0.1) {
		discard;
	}

	// --- 向量准备 ---
	vec3 N = normalize(normal);
    // [重要] 如果是渲染背面，则翻转法线以进行双面光照
    if (!gl_FrontFacing) {
        N = -N;
    }
    
    // [修正] 视图方向：从片元指向摄像机
	vec3 viewDir = normalize(cameraPos - worldPosition);
    // 光线方向：从片元指向光源
	vec3 lightDir = normalize(-globalLightDirection);

	// --- 光照计算 ---
	// 1. 标准漫反射
	float NdotL = max(dot(N, lightDir), 0.0);
	vec3 diffuse = NdotL * lightColor * albedo.rgb;

    // 2. (核心改进) 模拟透射光，让草叶背面也有光
    vec3 backLightDir = -lightDir;
    float transDot = max(dot(N, backLightDir), 0.0);
    vec3 translucency = pow(transDot, 2.0) * lightColor * albedo.rgb * u_translucency;

	// 3. 环境光
	vec3 ambientColor = ambient * albedo.rgb;
	
    // 最终颜色 = 环境光 + 漫反射 + 透射光
	vec3 finalColor = ambientColor + diffuse + translucency;

	FragColor = vec4(finalColor, albedo.a);
}