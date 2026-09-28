// Phase 10 批 10.7：GPU 点选高亮 frag（选中物体边缘发光）。
//
// 与 picking.frag 同机制但输出不同：picking 写 r32ui object_id 供回读，高亮写
// scene_hdr 的颜色输出（alpha 混合叠加在已渲好的画面上）。两者都是「克隆管线重放」
// 的 frag——mesh SPV 复用原条目，只换 frag SPV，故接口面必须是 ModelVertexOutput。
//
// **着色逻辑刻意极简**：菲涅尔边缘发光 = pow(1 - saturate(dot(N, V)), power)，
// 正对相机的面几乎不亮，掠射的轮廓边缘亮。这样高亮不遮挡材质本身的调参效果
// （调 roughness/metallic 时仍能看清表面高光变化），只勾出边界。
//
// 选中范围 = **整个材质**（同 MaterialRef.material 的全部实体一起亮），不是单个实例。
// 这个「哪些 draw 要亮」的判定在 CPU 侧（Renderer 按 materialId 过滤 draw list），
// 本 frag 只管「亮的那部分画成什么样」——不需要任何 per-object 选中标志进 scene data，
// 故不改 ObjectDraw 布局、不触发 just reflect 的字节契约重生成。
//
// binding/import 与 model_pbr_ibl.frag 对齐：需要 scene.slang 拿对象矩阵与 eyePos
// 算世界空间视向 V；纹理槽虽不消费，但克隆条目沿用原 bindings（映射空转无害，
// 同 picking.frag 与 skybox 条目先例）。
import model;     // ModelVertexOutput（mesh 输出接口匹配）
import material;  // push constant DrawArgs（objectIndex）
import scene;     // readSceneLight / readObjectMatrix / worldPos / worldNormal

// look-dev 常量（截图迭代）。
// **混合语义（关键，踩过一次坑）**：本 frag 输出走标准 alpha 混合
// （SRC_ALPHA / ONE_MINUS_SRC_ALPHA，见 shader.cppm colorBlend），合成结果 =
//   dst·(1−a) + src.rgb·a
// 所以 **a 才是「高亮权重」，src.rgb 只应是纯高亮色**。若把 rim 同时乘进 rgb 与 a
// （2026-09-28 首版写法），实际贡献退化为 rim²·intensity——power=4 时除轮廓线外
// rim≈0.05，rim²≈0.0025，实测点亮 1.8% 像素但通道最大差仅 17/255，肉眼不可见。
// 现改为 rgb = 纯色、a = saturate(rim)·intensity，贡献对 rim 线性。
static const float HIGHLIGHT_POWER = 2.0;     // 2 = 中等宽度轮廓（越大越细）
static const float HIGHLIGHT_INTENSITY = 1.6; // 总权重上限；>1 在 rgba16f HDR 域合法，
                                                // ACES tonemap 会压回但保留饱和亮边感
// **基色权重下限**（2026-09-28 实测踩坑）：纯菲涅尔只勾轮廓，而建筑墙面这类
// **正对相机的大平面** NdotV≈1 → rim≈0，点中后画面几乎无变化（实测仅 1.8% 像素
// 有差异、且全在轮廓线上，正面完全看不出点中了什么）。故加一层恒定基色权重
// BASE_WEIGHT 保证「点中即整体染色」，轮廓光只作强化。
static const float HIGHLIGHT_BASE_WEIGHT = 0.35;
static const float3 HIGHLIGHT_COLOR = float3(1.0, 0.55, 0.10);  // 琥珀色（与场景冷调互补）

[shader("fragment")]
float4 main(ModelVertexOutput input) : SV_Target
{
    // 视向 V 走世界空间（与全部 model frag 同约定）：frag 收到的是 object-space
    // 插值量，经对象矩阵表变换到世界——不能用屏幕空间导数近似（掠射角会算错）。
    ObjectMatrix M = readObjectMatrix(drawArgs.objectIndex);
    float3 P = worldPos(M, input.positionOS);
    float3 N = worldNormal(M, normalize(input.normal));
    float3 V = normalize(readSceneLight().eyePos - P);

    // 背面（双面材质如叶片）法线朝外翻，避免背面菲涅尔全黑导致叶片高亮缺失。
    float NdotV = abs(dot(N, V));
    float rim = pow(1.0 - saturate(NdotV), HIGHLIGHT_POWER);
    // 基色 + 轮廓：正面有基色（看得见选中），边缘叠轮廓光（看得清边界）
    float weight = saturate(HIGHLIGHT_BASE_WEIGHT + rim) * HIGHLIGHT_INTENSITY;

    // rgb 纯色 + a 承载权重（见上方「混合语义」）
    return float4(HIGHLIGHT_COLOR, weight);
}
