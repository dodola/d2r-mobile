#version 450
layout(set=0, binding=0) uniform sampler2D sourceDepth;
layout(push_constant) uniform CopyRegion {
    ivec2 sourceOffset;
    ivec2 destinationOffset;
    uvec2 sourceExtent;
    uvec2 destinationExtent;
    uint bitIndex;
} region;
void main() {
    uvec2 pixel = uvec2(ivec2(gl_FragCoord.xy) - region.destinationOffset);
    // Exact nearest texel selection at destination pixel centers, independent
    // of the guest viewport/origin. Host checks the multiplication range.
    uvec2 texel = ((pixel * 2u + 1u) * region.sourceExtent) / (region.destinationExtent * 2u);
    gl_FragDepth = texelFetch(sourceDepth, region.sourceOffset + ivec2(texel), 0).r;
}
