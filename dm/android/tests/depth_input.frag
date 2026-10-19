#version 450
#ifdef DEPTH_INPUT_EARLY_TESTS
layout(early_fragment_tests) in;
#endif
layout(input_attachment_index=0, set=0, binding=0) uniform subpassInput depth_input;
layout(location=0) flat in int primitive_index;
layout(location=0) out vec4 result;
void main() {
    float prior_depth = subpassLoad(depth_input).x;
    // Noncommuting affine updates distinguish all six primitive permutations.
    float next_depth = prior_depth * 0.5 + float(primitive_index) * 0.125;
    gl_FragDepth = next_depth;
    result = vec4(prior_depth, next_depth, float(primitive_index), 1.0);
}
