#version 460
#extension GL_EXT_mesh_shader : require

layout(max_vertices = 3, max_primitives = 1) out;
layout(triangles) out;
layout(local_size_x = 1) in;

void main()
{
    gl_MeshVerticesEXT[0].gl_Position = vec4(0.0, 0.5, 0.0, 1.0);
    gl_MeshVerticesEXT[1].gl_Position = vec4(-0.5, -0.5, 0.0, 1.0);
    gl_MeshVerticesEXT[2].gl_Position = vec4(0.5, -0.5, 0.0, 1.0);

    gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);

    SetMeshOutputsEXT(3, 1);
}
