local function Sampler(var, semantic, type, default, fallback)
    return {
        shader_variable = var,
        semantic = semantic,
        assimp_type = "aiTextureType_" .. type,
        fallback_assimp_key = fallback and ("aiTextureType_" .. fallback) or nil,
        default_texture = default
    }
end

return {
    UniformBlock = "MaterialIndices",
    
    Templates = {
        PBR_Standard = {
            Parameters = {
                -- 变量名, 语义, Assimp类型, 默认图, [回退Assimp类型]
                Sampler("base_color_texture", "base_color",        "BASE_COLOR",       "white", "DIFFUSE"),
                Sampler("normal_texture",     "normal_map",        "NORMALS",          "blue"),
                Sampler("roughness_texture",  "roughness_map",     "DIFFUSE_ROUGHNESS","white"),
                Sampler("ao_texture",         "ambient_occlusion_map", "AMBIENT_OCCLUSION", "white"),
                Sampler("metallic_texture",   "metallic_map",      "METALNESS",        "black"),
            }
        }
    }
}