#version 450
// Valid Vulkan shader, deliberately rejected by the input-only fixture gate.
layout(set=0, binding=0) uniform sampler2D depth_input;
layout(location=0) flat in int primitive_index;
layout(location=0) out vec4 result;
void main() {
    float prior_depth = textureLod(depth_input, gl_FragCoord.xy / vec2(8,6), 0.0).x;
    float next_depth = prior_depth * 0.5 + float(primitive_index) * 0.125;
    gl_FragDepth = next_depth;
    result = vec4(prior_depth, next_depth, float(primitive_index), 1.0);
}
