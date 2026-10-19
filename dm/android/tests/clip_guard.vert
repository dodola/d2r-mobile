#version 450
// Deliberately narrow proof shape: one entry block, explicit initialization,
// scalar Position writes and identical unescaped local z loads.
void main() {
    const float depths[4]=float[4](-0.25,0.25,0.75,1.25);
    float z=depths[gl_VertexIndex];
    gl_ClipDistance[0]=0.0;
    gl_ClipDistance[1]=0.0;
    gl_ClipDistance[2]=0.0;
    gl_ClipDistance[3]=0.0;
    gl_ClipDistance[4]=0.0;
    gl_ClipDistance[5]=0.0;
    gl_ClipDistance[6]=0.0;
    gl_ClipDistance[7]=0.0;
    gl_Position.x=(float(gl_VertexIndex)+0.5)/4.0*2.0-1.0;
    gl_Position.y=0.0;
    gl_Position.z=z;
    gl_Position.w=1.0;
    gl_PointSize=1.0;
    gl_ClipDistance[0]=z;
}
