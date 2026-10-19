#version 450
layout(set=0, binding=0, std430) buffer Results { uint values[]; } output_data;
layout(set=0, binding=1) uniform sampler2DShadow depth_2d;
layout(set=0, binding=2) uniform sampler2DArrayShadow depth_array;
void main() {
    uint slot = uint(gl_FragCoord.x);
    float reference[3] = float[3](0.25, 0.75, 0.5);
    float ref = reference[slot % 3u];
    // Both samples execute outside the selection, preserving implicit derivatives.
    float ordinary = texture(depth_2d, vec3(0.5, 0.5, ref));
    float arrayed = texture(depth_array, vec4(0.5, 0.5, 0.0, ref));
    output_data.values[slot] = uint(round((slot < 3u ? ordinary : arrayed) * 1000.0));
}
