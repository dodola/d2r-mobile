/* Java SAF transport is owned for the native queue's process lifetime. */
#include "cloud_android.h"
#include "../host/dm.h"
#include <jni.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    JavaVM *vm;
    jobject store;
    jclass file_class;
    jmethodID file_constructor,refresh,publish,cancel,close;
} cloud_java_binding;
static pthread_mutex_t binding_mu=PTHREAD_MUTEX_INITIALIZER;
static cloud_java_binding *binding;

static int java_error(JNIEnv *env) {
    if(!(*env)->ExceptionCheck(env))return 0;
    jthrowable failure=(*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    int error=EIO;
    const struct {const char *name;int error;} types[]={
        {"java/lang/SecurityException",EACCES},
        {"java/lang/OutOfMemoryError",ENOMEM},
        {"java/lang/IllegalArgumentException",EINVAL},
        {"java/util/concurrent/CancellationException",ECANCELED},
        {"java/io/InterruptedIOException",ETIMEDOUT}
    };
    for(unsigned i=0;i<sizeof types/sizeof types[0];i++) {
        jclass type=(*env)->FindClass(env,types[i].name);
        if(type) {int matches=(*env)->IsInstanceOf(env,failure,type);(*env)->DeleteLocalRef(env,type);if(matches){error=types[i].error;break;}}
        else (*env)->ExceptionClear(env);
    }
    if(failure)(*env)->DeleteLocalRef(env,failure);
    return error;
}
static JNIEnv *java_attach(cloud_java_binding *b,int *attached) {
    JNIEnv *env=NULL;*attached=0;
    int status=(*b->vm)->GetEnv(b->vm,(void **)&env,JNI_VERSION_1_6);
    if(status==JNI_OK)return env;
    if(status!=JNI_EDETACHED||(*b->vm)->AttachCurrentThread(b->vm,&env,NULL)!=JNI_OK)return NULL;
    *attached=1;return env;
}
static int java_transfer(const char *directory,uint64_t attempt,void *opaque,int publish) {
    cloud_java_binding *b=opaque;int attached;
    JNIEnv *env=java_attach(b,&attached);if(!env)return EIO;
    int error=0;
    if((*env)->PushLocalFrame(env,8)<0){java_error(env);error=ENOMEM;}
    else {
        jstring path=(*env)->NewStringUTF(env,directory);
        jobject file=path?(*env)->NewObject(env,b->file_class,b->file_constructor,path):NULL;
        if(!file) {java_error(env);error=ENOMEM;}
        else {(*env)->CallVoidMethod(env,b->store,publish?b->publish:b->refresh,file,(jlong)attempt);error=java_error(env);}
        (*env)->PopLocalFrame(env,NULL);
    }
    if(error)LOG("ANDROID_CLOUD_SAF operation=%s attempt=%llu error=%d",publish?"publish":"refresh",(unsigned long long)attempt,error);
    if(attached)(*b->vm)->DetachCurrentThread(b->vm);
    return error;
}
static int java_refresh(const char *directory,uint64_t attempt,void *opaque) {return java_transfer(directory,attempt,opaque,0);}
static int java_publish(const char *directory,uint64_t attempt,void *opaque) {return java_transfer(directory,attempt,opaque,1);}
static void java_cancel(uint64_t attempt,void *opaque) {
    cloud_java_binding *b=opaque;int attached;
    JNIEnv *env=java_attach(b,&attached);if(!env)return;
    (*env)->CallVoidMethod(env,b->store,b->cancel,(jlong)attempt);java_error(env);
    if(attached)(*b->vm)->DetachCurrentThread(b->vm);
}
static void binding_release(JNIEnv *env,cloud_java_binding *b) {
    if(!b)return;
    if(b->store&&b->close){(*env)->CallVoidMethod(env,b->store,b->close);java_error(env);}
    if(b->store)(*env)->DeleteGlobalRef(env,b->store);
    if(b->file_class)(*env)->DeleteGlobalRef(env,b->file_class);
    free(b);
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeCloudProvider(JNIEnv *env,jclass cls,jobject store,jstring provider,jstring cache) {
    (void)cls;
    /* Native configuration is serialized before nativeStart. Changing a tree
     * while playing is deliberately applied on the next process launch. */
    pthread_mutex_lock(&binding_mu);
    int error=0;cloud_java_binding *next=NULL;
    const char *id=NULL,*root=NULL;
    if(store) {
        if(!provider||!cache){error=EINVAL;goto done;}
        id=(*env)->GetStringUTFChars(env,provider,NULL);
        if(!id){java_error(env);error=ENOMEM;goto done;}
        root=(*env)->GetStringUTFChars(env,cache,NULL);
        if(!root){java_error(env);error=ENOMEM;goto done;}
        next=calloc(1,sizeof *next);if(!next){error=ENOMEM;goto done;}
        if((*env)->GetJavaVM(env,&next->vm)!=JNI_OK){error=EIO;goto done;}
        jclass type=(*env)->GetObjectClass(env,store);
        jclass file=NULL;
        if(!type){java_error(env);error=ENOMEM;goto methods_done;}
        file=(*env)->FindClass(env,"java/io/File");
        if(!type||!file){java_error(env);error=EINVAL;goto methods_done;}
        next->refresh=(*env)->GetMethodID(env,type,"refresh","(Ljava/io/File;J)V");
        if(java_error(env)){error=EINVAL;goto methods_done;}
        next->publish=(*env)->GetMethodID(env,type,"publish","(Ljava/io/File;J)V");
        if(java_error(env)){error=EINVAL;goto methods_done;}
        next->cancel=(*env)->GetMethodID(env,type,"cancel","(J)V");
        if(java_error(env)){error=EINVAL;goto methods_done;}
        next->close=(*env)->GetMethodID(env,type,"close","()V");
        if(java_error(env)){error=EINVAL;goto methods_done;}
        next->file_constructor=(*env)->GetMethodID(env,file,"<init>","(Ljava/lang/String;)V");
        if(java_error(env)){error=EINVAL;goto methods_done;}
        next->store=(*env)->NewGlobalRef(env,store);
        if(!next->store){java_error(env);error=ENOMEM;goto methods_done;}
        next->file_class=(*env)->NewGlobalRef(env,file);
        if(!next->file_class){java_error(env);error=ENOMEM;}
methods_done:
        if(type)(*env)->DeleteLocalRef(env,type);
        if(file)(*env)->DeleteLocalRef(env,file);
        if(error)goto done;
    }
    const dm_android_cloud_provider_ops ops={java_refresh,java_publish,java_cancel};
    error=dm_android_cloud_set_provider(store?id:NULL,store?root:NULL,store?&ops:NULL,next);
    if(!error){cloud_java_binding *old=binding;binding=next;next=NULL;binding_release(env,old);}
done:
    if(id)(*env)->ReleaseStringUTFChars(env,provider,id);
    if(root)(*env)->ReleaseStringUTFChars(env,cache,root);
    binding_release(env,next);
    pthread_mutex_unlock(&binding_mu);return error;
}
JNIEXPORT jstring JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeCloudStatus(JNIEnv *env,jclass cls) {
    (void)cls;dm_android_cloud_status status={0};int error=dm_android_cloud_status_get(&status);
    char text[512];
    snprintf(text,sizeof text,"{\"available\":%s,\"configured\":%s,\"enabled\":%s,\"busy\":%s,\"pending\":%s,\"active\":%s,\"error\":%d,\"attempt\":%llu,\"successful_attempts\":%llu}",
        error?"false":"true",status.configured?"true":"false",status.enabled?"true":"false",
        status.busy?"true":"false",status.pending?"true":"false",status.active?"true":"false",
        error?error:status.last_error,(unsigned long long)status.attempt,(unsigned long long)status.successful_attempts);
    return (*env)->NewStringUTF(env,text);
}
