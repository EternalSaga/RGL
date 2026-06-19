import types;

[shader("fragment")]
float4 main(VertexOutput input) : SV_Target
{
    // 1. 获取 Vulkan 硬件裁剪空间下的非线性深度 [0.0, 1.0]
    float nonLinearDepth = input.position.z;
    
    // 2. 填入你在 C++ 端设置的 Projection 矩阵的 Near 和 Far 值
    // ⚠️ 请务必与你 C++ 里的 glm::perspective(fov, aspect, near, far) 保持一致！
    float near = 0.1;
    float far = 10.0; // 如果你 C++ 里设的是 100.0，这里就改成 100.0
    
    // 3. 标准 Vulkan 深度线性化公式：还原出真实的“相机视点距离”（单位：米）
    float linearDepth = (near * far) / (far - nonLinearDepth * (far - near));
    
    // 4. 映射到可见光区间
    // 【方法 A】：映射整个视锥体范围（如果远平面很大，立方体细节可能不明显）
    // float visualDepth = (linearDepth - near) / (far - near);
    
    // 【方法 B（强烈推荐）】：根据你的相机位置动态“对焦”
    // 假设你的相机在 Z=3.0，立方体在原点，那么它的物理距离大概在 1.5米 到 4.5米 之间。
    // 我们只把 1.5m ~ 4.5m 的范围映射到 [0, 1] 的灰度上，这样对比度极高！
    float minVisualDist = 1.5;
    float maxVisualDist = 4.5;
    float visualDepth = (linearDepth - minVisualDist) / (maxVisualDist - minVisualDist);
    
    // 5. 防止数值溢出，强行限制在 [0.0, 1.0]
    visualDepth = clamp(visualDepth, 0.0, 1.0);
    
    // 输出灰度图：离相机近的角会很黑，远的角会很白
    return float4(visualDepth, visualDepth, visualDepth, 1.0);
}