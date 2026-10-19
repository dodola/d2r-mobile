#version 450
layout(location=0) flat out int primitive_index;
void main() {
    const vec2 positions[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    primitive_index = gl_VertexIndex / 3;
    gl_Position = vec4(positions[gl_VertexIndex % 3], 0.5, 1.0);
}
