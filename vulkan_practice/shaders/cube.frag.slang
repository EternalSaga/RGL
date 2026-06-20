import types;

[shader("fragment")]
float4 main(VertexOutput input) : SV_Target
{

    float nonLinearDepth = input.position.z;

    float near = 0.1;
    float far = 10.0; // 如果你 C++ 里设的是 100.0，这里就改成 100.0

    float linearDepth = (near * far) / (far - nonLinearDepth * (far - near));
    

    float minVisualDist = 1.5;
    float maxVisualDist = 4.5;
    float visualDepth = (linearDepth - minVisualDist) / (maxVisualDist - minVisualDist);
    
    // 5. 防止数值溢出，强行限制在 [0.0, 1.0]
    visualDepth = clamp(visualDepth, 0.0, 1.0);
    
    // 输出灰度图：离相机近的角会很黑，远的角会很白
    return float4(visualDepth, visualDepth, visualDepth, 1.0);
}