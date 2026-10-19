package org.d2r.binaryprobe;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;

/** Runs without an Activity, without bringing the game to background or accessing its files. */
public final class SafCloudInstrumentation extends Instrumentation {
    private int checks;
    interface Action{void run()throws Exception;}
    private void check(boolean value){checks++;if(!value)throw new AssertionError("check "+checks);}
    private void fails(Action action)throws Exception{try{action.run();throw new AssertionError("expected IOException");}catch(IOException expected){checks++;}}
    @Override public void onCreate(Bundle arguments){super.onCreate(arguments);start();}
    @Override public void onStart(){
        Bundle result=new Bundle();String report;
        try{run(getTargetContext());report="SAF_CONTENT_RESOLVER checks="+checks+" real_query=1 real_stream=1 loading=1 revoked=1 truncated=1 reliable_stream_error=1 deadline=1 journal_restart=1 unsupported_rename=1 passed=1";result.putString("stream",report+"\n");writeReport(report);finish(Activity.RESULT_OK,result);}
        catch(Throwable e){report="SAF_CONTENT_RESOLVER checks="+checks+" passed=0 error="+android.util.Log.getStackTraceString(e);result.putString("stream",report+"\n");writeReport(report);finish(Activity.RESULT_CANCELED,result);}
    }
    private void writeReport(String text){android.util.Log.i("SAF_CONTROLLED",text);try{Files.write(new File(getTargetContext().getFilesDir(),"saf-controlled-report.txt").toPath(),text.getBytes(StandardCharsets.UTF_8));}catch(IOException ignored){}}
    private File cache(File root,String name)throws IOException{File f=new File(root,name);if(!f.mkdirs()&&!f.isDirectory())throw new IOException("cache");return f;}
    private void run(Context context)throws Exception{
        File root=Files.createTempDirectory(context.getFilesDir().toPath(),"saf-run-").toFile();
        Uri one=DocumentsContract.buildTreeDocumentUri(ControlledDocumentsProvider.AUTHORITY,"one"),two=DocumentsContract.buildTreeDocumentUri(ControlledDocumentsProvider.AUTHORITY,"two");
        // Each instrumentation run gets distinct provider subtrees, retaining test data for inspection.
        String run="run-"+System.nanoTime();String docOne=DocumentsContract.getDocumentId(DocumentsContract.createDocument(context.getContentResolver(),DocumentsContract.buildDocumentUriUsingTree(one,"one"),DocumentsContract.Document.MIME_TYPE_DIR,run));
        one=DocumentsContract.buildTreeDocumentUri(ControlledDocumentsProvider.AUTHORITY,docOne);
        String docTwo=DocumentsContract.getDocumentId(DocumentsContract.createDocument(context.getContentResolver(),DocumentsContract.buildDocumentUriUsingTree(two,"two"),DocumentsContract.Document.MIME_TYPE_DIR,run));
        two=DocumentsContract.buildTreeDocumentUri(ControlledDocumentsProvider.AUTHORITY,docTwo);
        SafCloudStore store=new SafCloudStore(context,one);check(!store.providerId().equals(SafCloudStore.providerId(two)));
        byte[] first=SafCloudFormatTest.pack("Hero",100,false,"first"),rollback=SafCloudFormatTest.pack("Hero",1,false,"intentional restore");
        File a=cache(root,"a");store.refresh(a,1);check(a.list().length==0);Files.write(new File(a,"Hero.pack").toPath(),first);store.publish(a,1);
        File b=cache(root,"b");store.refresh(b,2);check(Arrays.equals(Files.readAllBytes(new File(b,"Hero.pack").toPath()),first));Files.write(new File(b,"Hero.pack").toPath(),rollback);store.publish(b,2);
        File c=cache(root,"c");store.refresh(c,3);check(Arrays.equals(Files.readAllBytes(new File(c,"Hero.pack").toPath()),rollback));
        long attempt=4;for(String fault:new String[]{"loading","error","revoked","truncated","streamerror"}){
            ControlledDocumentsProvider.fault=fault;File target=cache(root,"fail-"+fault);long n=attempt++;fails(()->store.refresh(target,n));check(target.list().length==0);
        }
        ControlledDocumentsProvider.fault="";File write=cache(root,"write-fail");long n=attempt++;store.refresh(write,n);Files.write(new File(write,"Hero.pack").toPath(),SafCloudFormatTest.pack("Hero",2,true,""));
        ControlledDocumentsProvider.fault="writeopen";fails(()->store.publish(write,n));store.close();
        ControlledDocumentsProvider.fault="";SafCloudStore restarted=new SafCloudStore(context,one);File recovered=cache(root,"recovered");int deletes=ControlledDocumentsProvider.deleteCalls;restarted.refresh(recovered,1);check(ControlledDocumentsProvider.deleteCalls==deletes+1);check(Arrays.equals(Files.readAllBytes(new File(recovered,"Hero.pack").toPath()),rollback));
        Files.write(new File(recovered,"Hero.pack").toPath(),SafCloudFormatTest.pack("Hero",2,true,""));restarted.publish(recovered,1);File tomb=cache(root,"tomb");restarted.refresh(tomb,2);check(SafCloudFormat.parsePack("Hero",Files.readAllBytes(new File(tomb,"Hero.pack").toPath())).tomb);restarted.close();
        SafCloudStore shortWait=new SafCloudStore(context,one,100);File late=cache(root,"late");ControlledDocumentsProvider.fault="delay";long start=System.nanoTime();fails(()->shortWait.refresh(late,1));check((System.nanoTime()-start)/1000000<350);Thread.sleep(650);check(late.list().length==0);shortWait.close();ControlledDocumentsProvider.fault="";
        SafCloudStore isolated=new SafCloudStore(context,two);File other=cache(root,"other");isolated.refresh(other,1);check(other.list().length==0);isolated.close();
        check(ControlledDocumentsProvider.renameCalls==0);check(ControlledDocumentsProvider.queryCalls>0&&ControlledDocumentsProvider.readCalls>0&&ControlledDocumentsProvider.writeCalls>0);
    }
}
