// izb_spike.frag.slang —— 1b 探针 fragment：恒出白（覆盖判据 = "非黑像素"，与颜色链路解耦）
[shader("fragment")]
float4 main() : SV_Target {
    return float4(1.0, 1.0, 1.0, 1.0);
}
