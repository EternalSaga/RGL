import model;
import material;  // 统一 push constant（DrawArgs 超集）
import scene;     // 场景级 bindless（binding 3：对象矩阵表，worldPos/worldNormal）

// Phase 7 D1：G-buffer 几何 pass fragment。复用 model.mesh.slang（同 ModelVertexOutput、
// 同绑定 binding0 顶点/1 baseColorTex/2 sampler/3 sceneData），**不做光照**——只把材质
// 几何/外观属性写进 4 个 MRT，供 light pass 光照。材质数据与 forward 严格同源
//（albedo = baseColor.rgb × baseColorTexture；法线 = 顶点法线 oct 编码；metal/rough =
// DrawArgs.roughnessMetallic 标量）——故 D1 判据 = deferred 与 forward 单灯着色等价。
// 法线贴图 / ORM 贴图采样 = Phase 8 纹理管线（引擎当前只带 baseColor 贴图 + 标量）。
//
// MRT 布局（= deferred 图 pass.colors 顺序 = color index，格式见 render_graph deferred desc）：
//   SV_Target0 albedo      RGBA8_SRGB         (linear 值写入，硬件 sRGB 编码)
//   SV_Target1 normal      RG16_SNORM         (octahedral 编码，范围 [-1,1])
//   SV_Target2 worldPos    RGBA32_SFLOAT      (世界空间位置，.xyz；RGBA32 系 RADV 对 RGB32_SFLOAT
//                                              采样报 FORMAT_NOT_SUPPORTED 的规避——spec 上 RGB32 可采样，见 devlog 勘误)
//   SV_Target3 metalRough  RGBA8_UNORM        (.r=metallic .g=roughness，[0,1] 直存 unorm)

// octahedral 编码（McTaggart 形态；与 light.frag octDecode 逐位对称——两端必须一致）
float2 octEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, -1.0, 1.0);
    return n.xy;
}

[shader("fragment")]
void main(
    ModelVertexOutput input,
    out float4 outAlbedo     : SV_Target0,
    out float2 outNormal     : SV_Target1,
    out float4 outWorldPos   : SV_Target2,
    out float4 outMetalRough : SV_Target3)
{
    float4 texel = baseColorTexture.Sample(baseColorSampler, input.uv);
    float3 albedo = drawArgs.baseColor.rgb * texel.rgb;

    ObjectMatrix M = readObjectMatrix(drawArgs.objectIndex);
    float3 N = worldNormal(M, normalize(input.normal));
    float3 P = worldPos(M, input.positionOS);

    outAlbedo = float4(albedo, 1.0);
    outNormal = octEncode(N);
    outWorldPos = float4(P, 0.0);
    // DrawArgs.roughnessMetallic: x=roughness y=metallic（material.slang 契约）
    outMetalRough = float4(clamp(drawArgs.roughnessMetallic.y, 0.0, 1.0),  // .r = metallic
                           clamp(drawArgs.roughnessMetallic.x, 0.0, 1.0),  // .g = roughness
                           0.0, 1.0);
}
