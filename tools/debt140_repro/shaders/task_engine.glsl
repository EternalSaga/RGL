#version 460
#extension GL_EXT_mesh_shader : enable
// §26 R1–R4 引擎形态 task（对照 shadow_meshlet.task.slang）：
//   row = rowBase(push.word0) + gl_DrawID；24B 行表读 word{0:gcX, 3:obj, 4:cstart, 5:mgc}；
//   payload 转交身份；EmitMeshTasksEXT(mgc,1,1) = 引擎 DispatchMesh(g,1,1)。
// -DDD=1（引擎 S3 的引擎外镜像）：行号 = gl_WorkGroupID.x，gcX==0 的行启动后早退。
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430, binding = 0) readonly buffer RowTable { uint t[]; } rowTbl; // 24B/行 = 6 word
layout(push_constant) uniform Push { uint rowBase; uint cascade; uint reserved; } push;
struct Payload { uint obj; uint cstart; uint mgc; };
taskPayloadSharedEXT Payload tpl;
void main() {
#ifdef DD
    uint row = push.rowBase + gl_WorkGroupID.x;
    if (rowTbl.t[row * 6u + 0u] == 0u)
        return; // 引擎 S3：MDI 下不可见行本就不启动 task；直派改为启动后早退
#else
    uint row = push.rowBase + gl_DrawID; // MDI 行号（引擎 SV_DrawIndex）
#endif
    uint mgc = rowTbl.t[row * 6u + 5u];
    tpl.obj = rowTbl.t[row * 6u + 3u];
    tpl.cstart = rowTbl.t[row * 6u + 4u];
    tpl.mgc = mgc;
    EmitMeshTasksEXT(mgc, 1u, 1u);
}
