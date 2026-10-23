#version 450
// 按窗口预旋转反求原图坐标，再应用裁剪和客体 Y 原点。
// Invert the window pre-rotation, then apply the crop and guest Y origin.
layout(set=0,binding=0) uniform sampler2D source_image;
layout(push_constant) uniform Region {
    vec4 crop;
    vec2 destination;
    uint rotation;
    uint flip_y;
} region;
layout(location=0) out vec4 color;
void main() {
    vec2 uv = gl_FragCoord.xy / region.destination;
    if (region.rotation == 1u) uv = vec2(uv.y, 1.0-uv.x);
    else if (region.rotation == 2u) uv = vec2(1.0)-uv;
    else if (region.rotation == 3u) uv = vec2(1.0-uv.y, uv.x);
    if (region.flip_y != 0u) uv.y = 1.0-uv.y;
    color = texture(source_image, region.crop.xy + uv * region.crop.zw);
}
