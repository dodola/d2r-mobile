/* Some Qualcomm compilers fail linking divergent OpGroupNonUniformBallot.
 * Form the same four-word mask by OR-reducing one predicate-selected lane bit.
 * Reduction includes exactly the active invocations at the original instruction;
 * no workgroup barrier, full-mask assumption, or fixed 32/64-lane assumption. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {uint32_t *words,next;size_t used,capacity;int failed;} DMBallotWriter;
static void dm_ballot_append(DMBallotWriter *out,const uint32_t *words,size_t count) {
    if(out->failed||count>out->capacity-out->used){out->failed=1;return;}
    memcpy(out->words+out->used,words,count*4);out->used+=count;
}
static uint32_t dm_ballot_value(DMBallotWriter *out,uint32_t op,uint32_t type,const uint32_t *args,size_t count) {
    uint32_t id=out->next++,header[]={(uint32_t)(count+3)<<16|op,type,id};
    dm_ballot_append(out,header,3);dm_ballot_append(out,args,count);return id;
}
#define DM_BALLOT_VALUE(out,op,type,...) dm_ballot_value(out,op,type,(uint32_t[]){__VA_ARGS__},sizeof((uint32_t[]){__VA_ARGS__})/4)

/* Owns/replaces heap bytes on success; caller retains ownership on failure.
 * Only one compute entry point and ordinary unsigned uvec4 ballots are accepted.
 * The runtime enables this pass only with compute subgroup arithmetic support. */
static uint8_t *dm_compute_ballot_apply(uint8_t *bytes,size_t *size,unsigned *replaced) {
    *replaced=0;if(*size<20||*size%4)return NULL;
    uint32_t *w=(uint32_t *)bytes;size_t words=*size/4,ballots=0;
    if(w[0]!=0x07230203||!w[3]||w[3]>1000000)return NULL;
    uint32_t uint_type=0,bool_type=0,vector_type=0,lane=0,pointer_type=0;
    unsigned entries=0;int arithmetic=0;
    for(size_t o=5;o<words;) {
        unsigned n=w[o]>>16,op=w[o]&65535;if(!n||n>words-o)return NULL;
        if(op==15){if(n<5||w[o+1]!=5)return NULL;entries++;}
        if(op==17&&n==2&&w[o+1]==63)arithmetic=1;
        if(op==20&&n==2)bool_type=w[o+1];
        if(op==21&&n==4&&w[o+2]==32&&!w[o+3])uint_type=w[o+1];
        if(op==71&&n==4&&w[o+2]==11&&w[o+3]==41)lane=w[o+1];
        if(op==339){if(n!=5)return NULL;ballots++;if(vector_type&&vector_type!=w[o+1])return NULL;vector_type=w[o+1];}
        o+=n;
    }
    if(!ballots)return bytes;
    if(entries!=1||!uint_type||!bool_type||ballots>100000)return NULL;
    int vector_valid=0,lane_valid=!lane;
    for(size_t o=5;o<words;o+=w[o]>>16){unsigned op=w[o]&65535,n=w[o]>>16;
        if(op==23&&n==4&&w[o+1]==vector_type&&w[o+2]==uint_type&&w[o+3]==4)vector_valid=1;
        if(op==32&&n==4&&w[o+2]==1&&w[o+3]==uint_type)pointer_type=w[o+1];
    }
    for(size_t o=5;o<words;o+=w[o]>>16)if((w[o]&65535)==59&&w[o]>>16==4&&w[o+2]==lane&&w[o+1]==pointer_type&&w[o+3]==1)lane_valid=1;
    if(!vector_valid||!lane_valid)return NULL;
    if(ballots>(SIZE_MAX/4-words-128)/128)return NULL;
    DMBallotWriter out={.capacity=words+ballots*128+128,.next=w[3],.used=5};
    out.words=malloc(out.capacity*4);if(!out.words)return NULL;memcpy(out.words,w,20);
    uint32_t constants[6],values[]={0,1,2,3,5,31};
    for(unsigned i=0;i<6;i++)constants[i]=out.next++;
    int new_lane=!lane,new_pointer=!pointer_type;
    if(new_lane){if(new_pointer)pointer_type=out.next++;lane=out.next++;}
    if(!arithmetic){uint32_t cap[]={2u<<16|17,63};dm_ballot_append(&out,cap,2);}
    int declared=0,decorated=0;
    for(size_t o=5;o<words;o+=w[o]>>16){unsigned n=w[o]>>16,op=w[o]&65535;
        if(new_lane&&op==15){
            uint32_t first=(n+1)<<16|15;dm_ballot_append(&out,&first,1);dm_ballot_append(&out,w+o+1,n-1);dm_ballot_append(&out,&lane,1);continue;
        }
        if(new_lane&&!decorated&&op>=19&&op<=39){uint32_t d[]={4u<<16|71,lane,11,41};dm_ballot_append(&out,d,4);decorated=1;}
        if(!declared&&op==54){
            for(unsigned i=0;i<6;i++){uint32_t c[]={4u<<16|43,uint_type,constants[i],values[i]};dm_ballot_append(&out,c,4);}
            if(new_lane){
                if(new_pointer){uint32_t p[]={4u<<16|32,pointer_type,1,uint_type};dm_ballot_append(&out,p,4);}
                uint32_t v[]={4u<<16|59,pointer_type,lane,1};dm_ballot_append(&out,v,4);
            }
            declared=1;
        }
        if(op!=339){dm_ballot_append(&out,w+o,n);continue;}
        uint32_t local=DM_BALLOT_VALUE(&out,61,uint_type,lane);
        uint32_t index=DM_BALLOT_VALUE(&out,199,uint_type,local,constants[5]);
        uint32_t bit=DM_BALLOT_VALUE(&out,196,uint_type,constants[1],index);
        uint32_t word=DM_BALLOT_VALUE(&out,194,uint_type,local,constants[4]),elements[4];
        for(unsigned i=0;i<4;i++){
            uint32_t part=DM_BALLOT_VALUE(&out,170,bool_type,word,constants[i]);
            uint32_t selected=DM_BALLOT_VALUE(&out,167,bool_type,part,w[o+4]);
            elements[i]=DM_BALLOT_VALUE(&out,169,uint_type,selected,bit,constants[0]);
        }
        uint32_t vector=dm_ballot_value(&out,80,vector_type,elements,4);
        uint32_t reduce[]={6u<<16|360,vector_type,w[o+2],w[o+3],0,vector};dm_ballot_append(&out,reduce,6);
        (*replaced)++;
    }
    if(out.failed||!declared||out.next>1000000){free(out.words);*replaced=0;return NULL;}
    out.words[3]=out.next;free(bytes);*size=out.used*4;return (uint8_t *)out.words;
}
#undef DM_BALLOT_VALUE
