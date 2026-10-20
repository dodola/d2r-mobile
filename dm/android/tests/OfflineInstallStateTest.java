package org.d2r.binaryprobe;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;

/** Host checks for the launcher's job policy, including real installer callbacks. */
public final class OfflineInstallStateTest {
    private static int checks;
    private static void check(boolean ok,String message){checks++;if(!ok)throw new AssertionError(message);}
    private static OfflineInstallState waiting(){
        OfflineInstallState state=new OfflineInstallState();state.finishWaiting();return state;
    }
    private static void stateTransitions()throws Exception {
        OfflineInstallState state=new OfflineInstallState();
        check(state.busy()&&!state.canSelect(),"Checking must block package selection");
        check(!state.requestCancel()&&!state.beginImport(),"Initial verification must not be cancelled or duplicated");
        state.finishWaiting();check(state.beginImport(),"Cannot begin import");
        check(!state.beginImport(),"Duplicate import accepted");
        check(state.requestCancel(),"Import could not be cancelled");
        check(!state.requestCancel()&&!state.canSelect()&&state.busy(),"Cancellation must wait for the worker");
        state.finishFailed();check(state.phase()==OfflineInstallState.Phase.STOPPED&&state.canSelect(),"Stopped import cannot retry");
        check(state.beginImport()&&!state.cancelled(),"Retry retained cancellation");
        state.finishFailed();check(state.phase()==OfflineInstallState.Phase.FAILED&&state.canSelect(),"Failure cannot retry");
        state.beginImport();state.beginCommit();
        check(!state.requestCancel()&&!state.beginImport()&&state.busy(),"Commit must block interruption");
        state.finishReady();check(state.ready()&&!state.busy()&&!state.canSelect(),"Ready must allow launch only");
        check(!state.requestCancel(),"Ready job was cancelled");
        // Startup can recover a durable commit without opening a package picker.
        OfflineInstallState recovery=new OfflineInstallState();recovery.beginCommit();
        check(recovery.busy()&&!recovery.requestCancel()&&!recovery.canSelect(),"Recovery must finish before selection");
        recovery.finishReady();
        // Duplicate selection results must enqueue exactly one job even when callers race.
        OfflineInstallState raced=waiting();CountDownLatch go=new CountDownLatch(1);AtomicInteger accepted=new AtomicInteger();
        Runnable start=()->{try{go.await();if(raced.beginImport())accepted.incrementAndGet();}catch(InterruptedException e){throw new AssertionError(e);}};
        Thread first=new Thread(start),second=new Thread(start);first.start();second.start();go.countDown();first.join();second.join();
        check(accepted.get()==1,"Concurrent starts queued multiple imports");
    }
    private static void installerCallbacks(boolean cancelAtCommit)throws Exception {
        byte[] payload=new byte[256*1024+17];
        byte[] index="shader-index".getBytes(StandardCharsets.US_ASCII);
        String hash=OfflinePackageInstaller.hex(MessageDigest.getInstance("SHA-256").digest(payload));
        String indexHash=OfflinePackageInstaller.hex(MessageDigest.getInstance("SHA-256").digest(index));
        OfflinePackageInstaller.Manifest manifest=new OfflinePackageInstaller.Manifest("{\"test\":1}".getBytes(StandardCharsets.UTF_8),
            Arrays.asList(new OfflinePackageInstaller.Entry("game.romfs",payload.length,hash),
                new OfflinePackageInstaller.Entry("d2r-mobile/shaders/shader_index.bin",index.length,indexHash)));
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(bytes);
        out.write(OfflinePackageInstaller.MAGIC);out.writeInt(manifest.json.length);out.write(manifest.json);out.write(payload);out.write(index);
        File root=Files.createTempDirectory("d2r-launcher-state-").toFile();OfflineInstallState state=waiting();state.beginImport();
        OfflinePackageInstaller installer=new OfflinePackageInstaller(root,manifest,(phase,done,total)->{
            if(phase.equals("commit")){
                // The cancellation click can race with a durable commit. Finishing the install wins.
                check(state.requestCancel(),"Race fixture did not request cancellation");state.beginCommit();
                check(!state.requestCancel(),"Commit accepted a second cancellation");
            }else if(!cancelAtCommit&&phase.equals("import"))state.requestCancel();
        },state::cancelled,()->Long.MAX_VALUE);
        if(cancelAtCommit){
            installer.install(new ByteArrayInputStream(bytes.toByteArray()));state.finishReady();
            check(state.ready()&&installer.prepareExisting(),"Cancellation at commit lost a complete install");
        }else{
            try{installer.install(new ByteArrayInputStream(bytes.toByteArray()));throw new AssertionError("Cancellation ignored");}
            catch(OfflinePackageInstaller.Failure e){check(e.reason.equals("cancelled"),"Wrong cancellation failure");state.finishFailed();}
            check(!new File(root,"d2r-offline-ready").exists()&&state.canSelect(),"Cancelled import is not retryable");
            check(state.beginImport(),"Cannot retry real cancelled import");
            OfflinePackageInstaller retry=new OfflinePackageInstaller(root,manifest,(phase,done,total)->{
                if(phase.equals("commit"))state.beginCommit();
            },state::cancelled,()->Long.MAX_VALUE);
            retry.install(new ByteArrayInputStream(bytes.toByteArray()));state.finishReady();
            check(state.ready()&&retry.prepareExisting(),"Retry did not finish");
        }
    }
    public static void main(String[] args)throws Exception {
        stateTransitions();installerCallbacks(false);installerCallbacks(true);
        System.out.println("OFFLINE_INSTALL_STATE checks="+checks+" passed=1 device_contacted=0");
    }
}
