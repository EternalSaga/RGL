local function Buffer(name, binding, readonly, optional)
    return {
        name = name,
        binding = binding,
        readonly = readonly,
        optional = optional or false
    }
end

return {
    Templates = {
        Standard_VertexPulling = {
            StorageBuffers = {
                Buffer("VertexBuffer", 2, true),
                Buffer("InstanceData", 1, false, true) 
            },
            Uniforms = {
                { name = "CameraBlock", binding = 0 }
            }
        }
    }
}