/* Apply the production pass to captured original modules without creating a GPU
 * device. Validate emitted modules separately with spirv-val. */
#include "nvn_vk_vertex_spv.h"
#include "nvn_vk_vertex.h"
#include <stdio.h>

int main(int argc,char **argv) {
    if(argc!=4)return 2;
    FILE *f=fopen(argv[1],"rb");if(!f)return 3;
    if(fseek(f,0,SEEK_END))return 4;long length=ftell(f);rewind(f);
    if(length<20||length>16*1024*1024)return 5;
    uint8_t *bytes=malloc((size_t)length);if(!bytes||fread(bytes,1,(size_t)length,f)!=(size_t)length)return 6;fclose(f);
    NvnVkVertexInput inputs[16];if(!nvn_vk_vertex_inputs((uint32_t *)bytes,(size_t)length/4,inputs))return 7;
    NvnVkVertexAdapt plan={0};uint32_t occupied=0;
    for(unsigned l=0;l<16;l++)if(inputs[l].type)occupied|=1u<<l;
    for(unsigned l=0;l<16;l++)if(inputs[l].type){
        if(inputs[l].components>=3){unsigned tail=0;while(tail<32&&(occupied&(1u<<tail)))tail++;if(tail==32)return 8;
            occupied|=1u<<tail;plan.split|=1u<<l;plan.tail[l]=tail;}
        if(atoi(argv[3])&&!inputs[l].kind)plan.convert[l]=(uint32_t)atoi(argv[3]);
    }
    unsigned active=plan.split;for(unsigned l=0;l<16;l++)active|=plan.convert[l];if(!active){free(bytes);return 77;}
    size_t size=(size_t)length;uint8_t *out=nvn_vk_vertex_adapt(bytes,&size,&plan);free(bytes);if(!out)return 9;
    f=fopen(argv[2],"wb");if(!f||fwrite(out,1,size,f)!=size)return 10;fclose(f);free(out);
    /* End-point arithmetic, including uint64 overflow and exact final byte. */
    if(!nvn_vk_vertex_extent(36,0,12,12,2)||nvn_vk_vertex_extent(35,0,12,12,2)||
       !nvn_vk_vertex_extent(36,24,12,0,UINT64_MAX)||nvn_vk_vertex_extent(36,25,12,0,0)||
       nvn_vk_vertex_extent(UINT64_MAX,0,16,16,UINT64_MAX))return 11;
    NvnVkVertexFormat rgb=nvn_vk_vertex_format(0x19),scaled=nvn_vk_vertex_format(0x62);
    if(rgb.bytes!=3||rgb.tail_offset!=2||rgb.tail!=VK_FORMAT_R8_UNORM||
       scaled.bytes!=12||scaled.convert!=2||scaled.head!=VK_FORMAT_R32G32_SINT||
       nvn_vk_vertex_scaled_native(0x5d,3)!=VK_FORMAT_R8G8B8_USCALED||
       nvn_vk_vertex_scaled_native(0x55,1)!=VK_FORMAT_UNDEFINED)return 12;
    return 0;
}
