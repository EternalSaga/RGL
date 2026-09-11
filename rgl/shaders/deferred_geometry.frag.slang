import model;
import material;  // 统一 push constant（DrawArgs 超集）
import scene;     // 场景级 bindless（binding 3：对象矩阵表，worldPos/worldNormal）
import normalmap;  // Phase 8 ③a：normal 贴图导数 TBN + DX flip-G 解码（binding 6/7 消费）

// Phase 7 D1：G-buffer 几何 pass fragment。复用 model.mesh.slang（同 ModelVertexOutput、
// 同绑定 binding0 顶点/1 baseColorTex/2 sampler/3 sceneData/6 normalTex/7 ormTex），
// **不做光照**——只把材质几何/外观属性写进 4 个 MRT，供 light pass 光照。材质数据与
// forward 严格同源（albedo = baseColor.rgb × baseColorTexture；法线 = 导数 TBN 后世界
// 法线 oct 编码（Phase 8 ③a normal 贴图，DX flip-G + Z 重建）；metal/rough = 材质 factor
// × ORM 贴图 B/G 通道（Phase 8 ③a））——故 D1 判据 = deferred 与 forward 单灯着色等价。
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
    const ObjectDraw od = readObjectDraw(drawArgs.objectIndex);  // Phase 7b+：材质进 heap
    float3 albedo = od.baseColor.rgb * texel.rgb;

    ObjectMatrix M = readObjectMatrix(drawArgs.objectIndex);
    // P 先于 N：导数 TBN 需要 dFdx/dFdy(世界位置)（Phase 8 ③a）
    float3 P = worldPos(M, input.positionOS);
    float3 N = worldNormal(M, normalize(input.normal));

    // Phase 8 ③a：ORM 贴图（G=roughness B=metallic，R=AO 采样不应用，白纹理回落 factor）
    // + normal 贴图（导数 TBN，DX flip-G + Z 重建）——公式与 forward model_pbr_ibl 严格
    // 同源（G-buffer 与 forward 等价性依赖此）
    float4 orm = ormTexture.Sample(baseColorSampler, input.uv);
    float3 tangentNormal = decodeNormalDx(normalTexture.Sample(baseColorSampler, input.uv));
    // Slang 导数内建 = ddx/ddy（GLSL 式 dFdx/dFdy 名不存在，本版本实测）
    N = applyNormalMap(N, ddx(P), ddy(P), ddx(input.uv), ddy(input.uv), tangentNormal);
    // ObjectDraw.roughnessMetallic: x=roughness y=metallic（scene.slang readObjectDraw）
    float metallic = clamp(od.roughnessMetallic.y * orm.b, 0.0, 1.0);
    float roughness = clamp(od.roughnessMetallic.x * orm.g, 0.04, 1.0);

    outAlbedo = float4(albedo, 1.0);
    outNormal = octEncode(N);
    outWorldPos = float4(P, 0.0);
    outMetalRough = float4(metallic, roughness, 0.0, 1.0);
}
