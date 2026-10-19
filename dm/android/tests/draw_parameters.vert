#version 450
#extension GL_ARB_shader_draw_parameters : require
layout(push_constant) uniform Case { int firstVertex; } testCase;
layout(location=0) flat out ivec4 parameters;
layout(location=1) flat out int instance;
void main() {
    parameters=ivec4(gl_BaseVertexARB,gl_BaseInstanceARB,gl_DrawIDARB,gl_VertexIndex);
    instance=gl_InstanceIndex;
    vec2 pixel=vec2(gl_VertexIndex-testCase.firstVertex,gl_InstanceIndex-gl_BaseInstanceARB)+0.5;
    gl_Position=vec4(pixel/vec2(3,2)*2.0-1.0,0,1);
    gl_PointSize=1.0;
}
