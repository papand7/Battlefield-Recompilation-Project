#version 450
// Full-size triangle covering the image centre; corners stay at the clear colour.
const vec2 kPos[3] = vec2[](vec2(0.0, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));
void main() { gl_Position = vec4(kPos[gl_VertexIndex], 0.0, 1.0); }
