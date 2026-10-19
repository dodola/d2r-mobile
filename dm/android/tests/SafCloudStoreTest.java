package org.d2r.binaryprobe;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

public final class SafCloudStoreTest {
    private static int checks;
    interface Action{void run()throws Exception;}
    static void check(boolean value){checks++;if(!value)throw new AssertionError("check "+checks);}
    static void fails(Action action)throws Exception{try{action.run();throw new AssertionError("expected failure");}catch(IOException expected){checks++;}}
    static final class Entry{final String id,parent,name;final boolean dir;byte[] bytes;Entry(String id,String parent,String name,boolean dir,byte[] bytes){this.id=id;this.parent=parent;this.name=name;this.dir=dir;this.bytes=bytes;}}
    static final class Provider implements SafCloudStore.Backend{
        final Map<String,Entry> entries=new LinkedHashMap<>();int next,creates,deletes;volatile String fault="";volatile long delay;boolean closeWrite;
        Provider(){entries.put("root",new Entry("root","","root",true,new byte[0]));entries.put("cloud",new Entry("cloud","root","D2R Mobile Cloud v1",true,new byte[0]));}
        public String root(){return "root";}
        public List<SafCloudStore.Doc> list(String parent,SafCloudStore.Session s)throws IOException{
            s.check();if(fault.equals("loading"))throw new IOException("loading");if(fault.equals("revoked"))throw new SecurityException("revoked");
            List<SafCloudStore.Doc> out=new ArrayList<>();synchronized(entries){for(Entry e:entries.values())if(e.parent.equals(parent))out.add(new SafCloudStore.Doc(e.id,e.name,e.dir,e.bytes.length));}return out;
        }
        public SafCloudStore.Doc create(String parent,String mime,String name,SafCloudStore.Session s)throws IOException{s.check();String id="n"+(++next);boolean dir=mime.equals("vnd.android.document/directory");synchronized(entries){entries.put(id,new Entry(id,parent,name,dir,new byte[0]));}creates++;return new SafCloudStore.Doc(id,name,dir,0);}
        public InputStream read(String id,SafCloudStore.Session s)throws IOException{
            if(delay>0){long end=System.nanoTime()+delay*1000000;while(System.nanoTime()<end){try{Thread.sleep(5);}catch(InterruptedException ignored){}}}s.check();
            byte[] source=entries.get(id).bytes.clone();if(fault.equals("truncate")&&source.length>0)source=Arrays.copyOf(source,source.length-1);
            final byte[] bytes=source;return new ByteArrayInputStream(bytes){@Override public void close()throws IOException{if(fault.equals("readclose"))throw new IOException("read close failed");super.close();}};
        }
        public OutputStream write(String id,SafCloudStore.Session s)throws IOException{
            return new ByteArrayOutputStream(){boolean done;@Override public synchronized void write(byte[] b,int off,int len){if(fault.equals("writepartial"))super.write(b,off,Math.min(len,12));else super.write(b,off,len);}
                @Override public synchronized void close()throws IOException{if(done)return;done=true;synchronized(entries){entries.get(id).bytes=toByteArray();}if(fault.equals("writeclose"))throw new IOException("write close failed");super.close();}};
        }
        public void delete(String id,SafCloudStore.Session s)throws IOException{s.check();if(fault.equals("delete"))throw new IOException("delete failed");entries.remove(id);deletes++;}
        void add(SafCloudFormat.Generation g)throws Exception{String name="g1-"+g.id+"-"+UUID.randomUUID().toString().replace("-","")+".swgen";byte[] data=SafCloudFormat.encode(g.unit,g.parents,g.pack.bytes);entries.put(name,new Entry(name,"cloud",name,false,data));}
    }
    static File cache(File root,String name)throws Exception{File f=new File(root,name);Files.createDirectories(f.toPath());return f;}
    static void put(File cache,byte[] pack)throws Exception{Files.write(new File(cache,"Hero.pack").toPath(),pack);}
    public static void main(String[] args)throws Exception{
        File root=Files.createTempDirectory("saf-cloud-controlled-").toFile();Provider p=new Provider();
        SafCloudFormat.Generation a=SafCloudFormatTest.generation("Hero",100,false,"A"),b=SafCloudFormatTest.generation("Hero",5,false,"B",a.id);p.add(a);
        File journal=new File(root,"journal/uploads"),first=cache(root,"a1");SafCloudStore store=new SafCloudStore(p,"test-provider",journal,3000);
        store.refresh(first,1);check(Arrays.equals(Files.readAllBytes(new File(first,"Hero.pack").toPath()),a.pack.bytes));store.publish(first,1);check(p.creates==0);
        fails(()->store.publish(first,1));fails(()->store.refresh(cache(root,"bad-reuse"),1));
        File second=cache(root,"a2");store.refresh(second,2);put(second,b.pack.bytes);store.publish(second,2);check(p.creates==1);
        File third=cache(root,"a3");store.refresh(third,3);check(Arrays.equals(Files.readAllBytes(new File(third,"Hero.pack").toPath()),b.pack.bytes));
        SafCloudFormat.Generation concurrent=SafCloudFormatTest.generation("Hero",200,false,"C",a.id);p.add(concurrent);
        File fourth=cache(root,"a4");store.refresh(fourth,4);check(Arrays.equals(Files.readAllBytes(new File(fourth,"Hero.pack").toPath()),concurrent.pack.bytes));check(new File(fourth,"conflicts/Hero/"+b.id+".pack").isFile());
        put(fourth,SafCloudFormatTest.pack("Hero",1,false,"rollback after both heads"));store.publish(fourth,4);
        File fifth=cache(root,"a5");store.refresh(fifth,5);check(SafCloudFormat.parsePack("Hero",Files.readAllBytes(new File(fifth,"Hero.pack").toPath())).stamp==1);check(!new File(fifth,"conflicts").exists());
        long attempt=6;
        for(String fault:new String[]{"loading","revoked","truncate","readclose"}){p.fault=fault;File f=cache(root,"fault-"+fault);long n=attempt++;fails(()->store.refresh(f,n));check(f.list().length==0);}
        p.fault="";File broken=cache(root,"write-broken");long brokenAttempt=attempt++;store.refresh(broken,brokenAttempt);put(broken,SafCloudFormatTest.pack("Hero",2,true,""));p.fault="writepartial";fails(()->store.publish(broken,brokenAttempt));check(journal.length()>0);int before=p.entries.size();store.close();
        p.fault="";SafCloudStore restarted=new SafCloudStore(p,"test-provider",journal,3000);File recovered=cache(root,"recovered");restarted.refresh(recovered,1);check(p.deletes==1&&p.entries.size()==before-1);check(!SafCloudFormat.parsePack("Hero",Files.readAllBytes(new File(recovered,"Hero.pack").toPath())).tomb);
        put(recovered,SafCloudFormatTest.pack("Hero",2,true,""));restarted.publish(recovered,1);File deletion=cache(root,"deletion");restarted.refresh(deletion,2);check(SafCloudFormat.parsePack("Hero",Files.readAllBytes(new File(deletion,"Hero.pack").toPath())).tomb);
        put(deletion,SafCloudFormatTest.pack("Hero",0,false,"restore older"));p.fault="writeclose";fails(()->restarted.publish(deletion,2));int deleted=p.deletes;p.fault="";File completeAfterError=cache(root,"complete-after-error");restarted.refresh(completeAfterError,3);check(p.deletes==deleted);check(SafCloudFormat.parsePack("Hero",Files.readAllBytes(new File(completeAfterError,"Hero.pack").toPath())).stamp==0);
        restarted.cancel(3);fails(()->restarted.publish(completeAfterError,3));restarted.close();
        SafCloudStore timeout=new SafCloudStore(p,"test-provider",new File(root,"timeout/uploads"),80);File late=cache(root,"late");p.delay=400;long start=System.nanoTime();fails(()->timeout.refresh(late,1));check((System.nanoTime()-start)/1000000<250);Thread.sleep(450);check(late.list().length==0);timeout.close();p.delay=0;
        // An unknown incomplete object is never interpreted as an empty cloud or deleted.
        String bad="g1-"+String.join("",Collections.nCopies(64,"a"))+"-"+String.join("",Collections.nCopies(32,"b"))+".swgen";p.entries.put(bad,new Entry(bad,"cloud",bad,false,new byte[0]));
        SafCloudStore unknown=new SafCloudStore(p,"test-provider",new File(root,"unknown/uploads"),3000);File failEmpty=cache(root,"unknown-cache");fails(()->unknown.refresh(failEmpty,1));check(failEmpty.list().length==0&&p.entries.containsKey(bad));unknown.close();
        System.out.println("SAF_STORE checks="+checks+" rollback=1 retained_conflicts=1 stream_close=1 partial=1 journal_restart=1 deadline_late_callback=1 passed=1");
    }
}
