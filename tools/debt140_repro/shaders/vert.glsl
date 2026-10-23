#version 450
// interleave 用经典顶点管线：全屏三角形（队列里 MDI 之外的活）
vec2 pos[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main() { gl_Position = vec4(pos[gl_VertexIndex], 0.5, 1.0); }
