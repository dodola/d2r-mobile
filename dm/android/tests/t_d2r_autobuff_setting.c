/* Execute the actual source policy, with settings and title-attach inputs.
 * Private gameplay bodies are discarded by the linker, never exercised. */
#include "../../host/dm_autobuff.c"
static int setting=1,verified;
int dm_custom_get(const char *key){if(strcmp(key,"auto_buff"))abort();return setting;}
int dm_android_title_custom_settings(void){return verified;}
int main(int argc,char **argv){
    if(argc!=2)return 2;int force=atoi(argv[1]),checks=0;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"autobuff setting failed line=%d\n",__LINE__);return 1;}}while(0)
    if(force<0)unsetenv("DM_AUTOBUFF");else setenv("DM_AUTOBUFF",force?"1":"0",1);
    CHECK(enabled()==(force>0));verified=1;
    CHECK(enabled()==(force<0?1:force));setting=0;CHECK(enabled()==(force>0));setting=1;CHECK(enabled()==(force<0?1:force));
    printf("ANDROID_D2R_AUTOBUFF_SETTING_OK checks=%d force=%d default_apply_off_on=1 title_guard=1 explicit_dev_override=1 original_game_not_claimed=1\n",checks,force);return 0;
}
