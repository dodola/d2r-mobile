package org.d2r.binaryprobe;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Map;

public final class SafCloudFormatTest {
    private static int checks;
    interface Action{void run()throws Exception;}
    static void check(boolean value){checks++;if(!value)throw new AssertionError("check "+checks);}
    static void fails(Action action)throws Exception{try{action.run();throw new AssertionError("expected IOException");}catch(IOException expected){checks++;}}
    public static byte[] pack(String unit,long stamp,boolean tomb,String content){
        byte[] path=(unit.equals("_stash")?"SharedStashSoftCoreV2.d2i":unit+"/"+unit+".d2s").getBytes(StandardCharsets.UTF_8),data=content.getBytes(StandardCharsets.UTF_8);
        ByteBuffer b=ByteBuffer.allocate(tomb?24:24+2+path.length+8+4+data.length).order(ByteOrder.LITTLE_ENDIAN);
        b.put(new byte[]{'S','W','P','K','1','\n',0,0}).putLong(stamp).putInt(tomb?1:0).putInt(tomb?0:1);
        if(!tomb)b.putShort((short)path.length).put(path).putLong(stamp).putInt(data.length).put(data);return b.array();
    }
    static SafCloudFormat.Generation generation(String unit,long stamp,boolean tomb,String content,String...parents)throws Exception{return SafCloudFormat.decode(SafCloudFormat.encode(unit,Arrays.asList(parents),pack(unit,stamp,tomb,content)));}
    public static void main(String[] args)throws Exception{
        check(!SafCloudFormat.providerId("ab","c").equals(SafCloudFormat.providerId("a","bc")));
        check(SafCloudFormat.providerId("provider","root:你好").equals(SafCloudFormat.providerId("provider","root:你好")));
        for(String unit:new String[]{"../X","_other","a/b","a\\b",".","a\tb","a:b",String.join("",Collections.nCopies(22,"汉"))})fails(()->SafCloudFormat.validUnit(unit));
        SafCloudFormat.Generation a=generation("Hero",100,false,"A");
        byte[] envelope=SafCloudFormat.encode("Hero",Collections.singletonList(a.id),pack("Hero",5,false,"B"));
        SafCloudFormat.Generation b=SafCloudFormat.decode(envelope);check(b.pack.stamp==5&&b.parents.equals(Collections.singletonList(a.id)));
        for(int i=0;i<envelope.length;i++){byte[] truncated=Arrays.copyOf(envelope,i);fails(()->SafCloudFormat.decode(truncated));}
        byte[] bad=envelope.clone();bad[bad.length-1]^=1;fails(()->SafCloudFormat.decode(bad));
        byte[] hashBad=envelope.clone();hashBad[65]^=1;fails(()->SafCloudFormat.decode(hashBad));
        byte[] extra=Arrays.copyOf(envelope,envelope.length+1);fails(()->SafCloudFormat.decode(extra));
        byte[] wrong=pack("Other",1,false,"x");fails(()->SafCloudFormat.parsePack("Hero",wrong));
        byte[] tombFiles=pack("Hero",1,false,"x");ByteBuffer.wrap(tombFiles).order(ByteOrder.LITTLE_ENDIAN).putInt(16,1);fails(()->SafCloudFormat.parsePack("Hero",tombFiles));
        byte[] flags=pack("Hero",1,true,"");ByteBuffer.wrap(flags).order(ByteOrder.LITTLE_ENDIAN).putInt(16,3);fails(()->SafCloudFormat.parsePack("Hero",flags));
        fails(()->SafCloudFormat.encode("Hero",Arrays.asList(a.id,a.id),pack("Hero",1,true,"")));
        Map<String,SafCloudFormat.Unit> resolved=SafCloudFormat.resolve(Arrays.asList(a,b));check(resolved.get("Hero").selected.id.equals(b.id));
        SafCloudFormat.Generation c=generation("Hero",200,false,"C",a.id);
        resolved=SafCloudFormat.resolve(Arrays.asList(c,a,b));check(resolved.get("Hero").heads.size()==2&&resolved.get("Hero").selected.id.equals(c.id));
        SafCloudFormat.Generation rollback=generation("Hero",1,false,"restored older save",b.id,c.id);
        resolved=SafCloudFormat.resolve(Arrays.asList(a,b,c,rollback));check(resolved.get("Hero").heads.size()==1&&resolved.get("Hero").selected.id.equals(rollback.id));
        SafCloudFormat.Generation deletion=generation("Hero",1,true,"",rollback.id),restore=generation("Hero",0,false,"restore after deletion",deletion.id);
        resolved=SafCloudFormat.resolve(Arrays.asList(a,b,c,rollback,deletion,restore));check(resolved.get("Hero").selected.id.equals(restore.id));
        SafCloudFormat.Generation tombTie=generation("Hero",100,true,"");resolved=SafCloudFormat.resolve(Arrays.asList(a,tombTie));check(resolved.get("Hero").selected.pack.tomb);
        SafCloudFormat.Generation unsigned=generation("Hero",Long.MIN_VALUE,false,"future");resolved=SafCloudFormat.resolve(Arrays.asList(a,unsigned));check(resolved.get("Hero").selected.id.equals(unsigned.id));
        fails(()->SafCloudFormat.resolve(Collections.singletonList(b)));
        SafCloudFormat.Generation other=generation("Other",1,false,"x",a.id);fails(()->SafCloudFormat.resolve(Arrays.asList(a,other)));
        List<SafCloudFormat.Generation> list=new ArrayList<>(Arrays.asList(a,b,c));String winner=SafCloudFormat.resolve(list).get("Hero").selected.id;Collections.reverse(list);check(SafCloudFormat.resolve(list).get("Hero").selected.id.equals(winner));
        System.out.println("SAF_FORMAT checks="+checks+" causal_rollback=1 concurrent_heads=1 completion_hash=1 invalid_paths=1 passed=1");
    }
}
