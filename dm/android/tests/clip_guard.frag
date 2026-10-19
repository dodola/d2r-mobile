#version 450
layout(set=0,binding=0,std430) buffer Results { uint values[]; } results;
void main() { results.values[int(gl_FragCoord.x)]=1u; }
