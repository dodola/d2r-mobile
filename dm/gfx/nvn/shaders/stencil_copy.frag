#version 450
layout(set=0, binding=1) uniform usampler2D sourceStencil;
layout(push_constant) uniform CopyRegion {
    ivec2 sourceOffset;
    ivec2 destinationOffset;
    uvec2 sourceExtent;
    uvec2 destinationExtent;
    uint bitIndex;
} region;
void main() {
    uvec2 pixel = uvec2(ivec2(gl_FragCoord.xy) - region.destinationOffset);
    uvec2 texel = ((pixel * 2u + 1u) * region.sourceExtent) / (region.destinationExtent * 2u);
    uint value = texelFetch(sourceStencil, region.sourceOffset + ivec2(texel), 0).r;
    if ((value & (1u << region.bitIndex)) == 0u) discard;
}
