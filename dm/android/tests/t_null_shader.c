/* Host adapter for spirv-val checks against real, externally supplied shaders. */
#include "../../gfx/nvn/nvn_vk_null_shader.h"
#include <stdio.h>
#include <assert.h>
static void unit_tests(void) {
    const uint32_t module[]={0x07230203,0x10300,0,20,0,
        4u<<16|71,4,33,3,4u<<16|21,1,32,0,4u<<16|32,2,2,1,
        4u<<16|59,2,4,2,2u<<16|19,5,3u<<16|33,6,5,
        5u<<16|54,5,7,0,6,2u<<16|248,8,4u<<16|61,1,9,4,1u<<16|253,1u<<16|56};
    uint32_t *out=NULL,binding=3;size_t n=0;const char *error=NULL;
    assert(nvn_vk_null_shader(module,sizeof(module)/4,&binding,1,&out,&n,&error));
    unsigned loads=0,copies=0,zeros=0;for(size_t at=5;at<n;at+=out[at]>>16){loads+=(out[at]&65535)==61;copies+=(out[at]&65535)==83;zeros+=(out[at]&65535)==46;}
    assert(!loads&&copies==1&&zeros==1);free(out);
    binding=99;assert(nvn_vk_null_shader(module,sizeof(module)/4,&binding,1,&out,&n,&error));assert(n==sizeof(module)/4&&!memcmp(out,module,sizeof module));free(out);
    uint32_t broken[sizeof(module)/4];memcpy(broken,module,sizeof module);broken[5]=0;
    assert(!nvn_vk_null_shader(broken,sizeof(module)/4,&binding,1,&out,&n,&error));assert(!out);
    memcpy(broken,module,sizeof module);broken[33]=(4u<<16)|57; /* unsupported pointer escape */
    binding=3;assert(!nvn_vk_null_shader(broken,sizeof(module)/4,&binding,1,&out,&n,&error));assert(!out);
    puts("null shader bounds, zero reads, unchanged live resources and pointer escape checks passed");
}
int main(int argc,char **argv) {
    if(argc==1){unit_tests();return 0;}
    if(argc<4)return 2;
    FILE *f=fopen(argv[1],"rb");if(!f)return 3;
    if(fseek(f,0,SEEK_END))return 3;long bytes=ftell(f);rewind(f);
    if(bytes<20||bytes%4)return 3;
    uint32_t *input=malloc(bytes),*output=NULL;size_t words=0;
    if(!input||fread(input,1,bytes,f)!=(size_t)bytes)return 3;fclose(f);
    uint32_t bindings[64];if(argc-3>64)return 2;
    for(int i=3;i<argc;i++)bindings[i-3]=(uint32_t)strtoul(argv[i],NULL,0);
    const char *error=NULL;
    if(!nvn_vk_null_shader(input,(size_t)bytes/4,bindings,(size_t)argc-3,&output,&words,&error)){
        fprintf(stderr,"%s\n",error);free(input);return 4;
    }
    f=fopen(argv[2],"wb");if(!f||fwrite(output,4,words,f)!=words||fclose(f))return 3;
    free(input);free(output);return 0;
}
