// mdi_spike.frag.slang —— 探针 fragment：把 mesh 平摊来的 tag 原样写进 R32_UINT 附件
//
// 走**整数**附件而不是颜色附件，理由：tag 是判据本体，任何量化/取整/sRGB/ACES 链路都会
// 让"值与列不符"和"链路有损"两种解释分不开。整数回环已由 tests/test_device_primitives.cppm
// （R32_UINT clear 0xCAFEBABE 逐像素精确）单独担保，本探针因此只欠 DrawIndex 这一个变量。
struct In
{
    float4 position : SV_Position;
    nointerpolation uint tag : TEXCOORD0;
};

[shader("fragment")]
uint main(In i) : SV_Target
{
    return i.tag;
}
