#version 460
#extension GL_EXT_mesh_shader : require

layout(max_vertices = 3, max_primitives = 1) out;
layout(triangles) out;
layout(local_size_x = 1) in;

// mesh 输出接口必须是数组（每元素对应一个顶点）
layout(location = 0) out vec2 vUv[3];

void main()
{
    // Fullscreen triangle in clip space: corners (-1,-1) (3,-1) (-1,3)
    // uv = (ndc + 1) / 2
    gl_MeshVerticesEXT[0].gl_Position = vec4(-1.0, -1.0, 0.0, 1.0);
    gl_MeshVerticesEXT[1].gl_Position = vec4( 3.0, -1.0, 0.0, 1.0);
    gl_MeshVerticesEXT[2].gl_Position = vec4(-1.0,  3.0, 0.0, 1.0);
    vUv[0] = vec2(0.0, 0.0);
    vUv[1] = vec2(2.0, 0.0);
    vUv[2] = vec2(0.0, 2.0);
    gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
    SetMeshOutputsEXT(3, 1);
}
