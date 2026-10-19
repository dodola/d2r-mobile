#version 450
layout(set=0,binding=0,std430) buffer Results { int values[]; } results;
layout(location=0) flat in ivec4 parameters;
layout(location=1) flat in int instance;
void main() {
    int slot=(int(gl_FragCoord.y)*3+int(gl_FragCoord.x))*5;
    results.values[slot+0]=parameters.x;
    results.values[slot+1]=parameters.y;
    results.values[slot+2]=parameters.z;
    results.values[slot+3]=parameters.w;
    results.values[slot+4]=instance;
}
