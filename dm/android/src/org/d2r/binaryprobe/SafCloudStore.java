package org.d2r.binaryprobe;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.UUID;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/** Serial, bounded SAF transport. Success acknowledges a provider readback, not its remote server. */
public final class SafCloudStore implements Closeable {
    private static final String DIRECTORY="D2R Mobile Cloud v1",SUFFIX=".swgen";
    private static final long DEADLINE_MS=15000,MAX_LIST_BYTES=256L*1024*1024;
    private static final ThreadFactory DAEMON=r->{Thread t=new Thread(r,"saf-cloud-io");t.setDaemon(true);return t;};
    private static final ExecutorService ABORT=Executors.newCachedThreadPool(DAEMON);
    private final Object lock=new Object();
    private final ExecutorService worker=Executors.newSingleThreadExecutor(DAEMON);
    private final Backend backend;
    private final String provider;
    private final File journal;
    private final Thread uiThread;
    private final long timeoutMillis;
    private final boolean durable;
    private long lastAttempt;
    private Session current;
    private boolean closed;

    public SafCloudStore(Context context,Uri tree)throws IOException{
        this(context,tree,DEADLINE_MS);
    }
    SafCloudStore(Context context,Uri tree,long timeoutMillis)throws IOException{
        this(new AndroidBackend(context.getApplicationContext().getContentResolver(),tree),providerId(tree),
            new File(new File(context.getApplicationContext().getFilesDir(),"saf-cloud"),providerId(tree)+".uploads"),
            Looper.getMainLooper().getThread(),timeoutMillis,true);
    }
    private SafCloudStore(Backend backend,String provider,File journal,Thread uiThread,long timeoutMillis,boolean durable){
        this.backend=backend;this.provider=provider;this.journal=journal;this.uiThread=uiThread;this.timeoutMillis=timeoutMillis;this.durable=durable;
    }
    /** A controlled provider seam uses the same transaction, stream and cancellation policy. */
    SafCloudStore(Backend backend,String provider,File journal,long timeoutMillis){this(backend,provider,journal,null,timeoutMillis,false);}
    public static String providerId(Uri tree)throws IOException{
        if(tree==null||!"content".equals(tree.getScheme())||tree.getAuthority()==null||!DocumentsContract.isTreeUri(tree))throw new IOException("A SAF tree URI is required");
        try{return SafCloudFormat.providerId(tree.getAuthority(),DocumentsContract.getTreeDocumentId(tree));}
        catch(IllegalArgumentException e){throw new IOException("Invalid SAF tree URI",e);}
    }
    public String providerId(){return provider;}
    private void offUi(){if(Thread.currentThread()==uiThread)throw new IllegalStateException("SAF I/O must run off the UI thread");}
    public void refresh(File cache,long attempt)throws IOException{
        offUi();if(cache==null)throw new IOException("Missing cache");File canonical=cache.getCanonicalFile();
        Session s;
        synchronized(lock){
            if(closed||attempt==0||Long.compareUnsigned(attempt,lastAttempt)<=0)throw new IOException("Stale SAF attempt");
            if(current!=null)invalidate(current);lastAttempt=attempt;s=new Session(attempt,canonical,System.nanoTime()+TimeUnit.MILLISECONDS.toNanos(timeoutMillis));current=s;
        }
        execute(s,()->{refreshWorker(s);return null;});
    }
    public void publish(File cache,long attempt)throws IOException{
        offUi();if(cache==null)throw new IOException("Missing cache");Session s;
        synchronized(lock){s=current;if(s==null||s.attempt!=attempt||!s.refreshed||s.publishStarted||!s.cache.equals(cache.getCanonicalFile()))throw new IOException("Stale/unrefreshed SAF publication");check(s);s.publishStarted=true;}
        execute(s,()->{publishWorker(s);return null;});
    }
    public void cancel(long attempt){synchronized(lock){if(current!=null&&current.attempt==attempt)invalidate(current);}}
    @Override public void close(){synchronized(lock){closed=true;if(current!=null)invalidate(current);}worker.shutdownNow();}
    private void invalidate(Session s){
        s.cancelled=true;if(s.future!=null)s.future.cancel(true);
        for(Runnable abort:s.aborts)ABORT.execute(abort);s.aborts.clear();
    }
    private void check(Session s)throws IOException{
        synchronized(lock){if(closed||current!=s||s.cancelled||Thread.currentThread().isInterrupted()||System.nanoTime()-s.deadline>=0)throw new IOException("SAF attempt cancelled or timed out");}
    }
    private void onCancel(Session s,Runnable abort)throws IOException{
        synchronized(lock){check(s);s.aborts.add(abort);}
    }
    private <T>T execute(Session s,Callable<T> operation)throws IOException{
        Future<T> f=worker.submit(()->{check(s);return operation.call();});
        synchronized(lock){s.future=f;if(s.cancelled)f.cancel(true);}
        try{T result=f.get(Math.max(1,s.deadline-System.nanoTime()),TimeUnit.NANOSECONDS);check(s);return result;}
        catch(TimeoutException|java.util.concurrent.CancellationException e){synchronized(lock){invalidate(s);}throw new IOException("SAF provider timed out/cancelled",e);}
        catch(InterruptedException e){synchronized(lock){invalidate(s);}Thread.currentThread().interrupt();throw new IOException("SAF wait interrupted",e);}
        catch(ExecutionException e){synchronized(lock){invalidate(s);}Throwable cause=e.getCause();if(cause instanceof IOException)throw (IOException)cause;throw new IOException("SAF provider failed",cause);}
    }
    static final class Doc {
        final String id,name;final boolean directory;final long size;
        Doc(String id,String name,boolean directory,long size){this.id=id;this.name=name;this.directory=directory;this.size=size;}
    }
    interface Backend {
        String root();
        List<Doc> list(String parent,Session attempt)throws IOException;
        Doc create(String parent,String mime,String name,Session attempt)throws IOException;
        InputStream read(String id,Session attempt)throws IOException;
        OutputStream write(String id,Session attempt)throws IOException;
        void delete(String id,Session attempt)throws IOException;
    }
    final class Session {
        final long attempt,deadline;final File cache;final List<Runnable> aborts=new ArrayList<>();
        volatile boolean cancelled;Future<?> future;boolean refreshed,publishStarted;
        Map<String,SafCloudFormat.Unit> units;List<Doc> directories;
        Session(long attempt,File cache,long deadline){this.attempt=attempt;this.cache=cache;this.deadline=deadline;}
        void check()throws IOException{SafCloudStore.this.check(this);}
        void onCancel(Runnable abort)throws IOException{SafCloudStore.this.onCancel(this,abort);}
    }
    private List<Doc> directories(Session s)throws IOException{
        List<Doc> out=new ArrayList<>();for(Doc d:backend.list(backend.root(),s)){check(s);if(DIRECTORY.equals(d.name)){if(!d.directory)throw new IOException("Cloud directory name occupied by a file");out.add(d);}}
        Collections.sort(out,(a,b)->a.id.compareTo(b.id));return out;
    }
    private static boolean generationName(String name){return name!=null&&name.matches("g1-[0-9a-f]{64}-[0-9a-f]{32}\\.swgen");}
    private byte[] readDocument(Doc d,Session s)throws IOException{
        if(d.directory||d.size>SafCloudFormat.MAX_GENERATION_BYTES)throw new IOException("Invalid generation document");
        byte[] bytes;try(InputStream in=backend.read(d.id,s)){if(in==null)throw new IOException("Provider returned no stream");s.onCancel(()->quietClose(in));bytes=readAll(in,SafCloudFormat.MAX_GENERATION_BYTES,s);}
        check(s);if(d.size>=0&&bytes.length!=d.size)throw new IOException("Partial or concurrently changing provider read");return bytes;
    }
    private static byte[] readAll(InputStream in,int max,Session s)throws IOException{
        ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] buffer=new byte[32768];int n;
        for(;;){s.check();n=in.read(buffer);if(n<0)break;if(n==0)throw new IOException("Provider made no read progress");if(out.size()>max-n)throw new IOException("Provider object too large");out.write(buffer,0,n);}s.check();return out.toByteArray();
    }
    private void refreshWorker(Session s)throws IOException{
        check(s);if(!s.cache.isDirectory()||Files.isSymbolicLink(s.cache.toPath())||listFiles(s.cache).length!=0)throw new IOException("Refresh requires an existing empty private cache");
        Map<String,String> uploads=loadJournal();List<Doc> dirs=directories(s);List<Doc> documents=new ArrayList<>();Set<String> docIds=new HashSet<>();
        for(Doc dir:dirs)for(Doc d:backend.list(dir.id,s)){check(s);if((generationName(d.name)||uploads.containsKey(d.name)||uploads.containsValue(d.id))&&docIds.add(d.id))documents.add(d);}
        if(documents.size()>SafCloudFormat.MAX_GENERATIONS)throw new IOException("Cloud generation limit reached");
        List<SafCloudFormat.Generation> all=new ArrayList<>();long total=0;Set<String> removedIds=new HashSet<>(),completedUploads=new HashSet<>();
        for(Doc d:documents){
            byte[] bytes=readDocument(d,s);total+=bytes.length;if(total>MAX_LIST_BYTES)throw new IOException("Cloud listing byte limit reached");
            SafCloudFormat.Generation g;
            try{g=SafCloudFormat.decode(bytes);if(!d.name.startsWith("g1-"+g.id+"-"))throw new IOException("Generation filename/hash mismatch");}
            catch(IOException invalid){
                // Only a durable pre-create journal proves that an invalid object is our failed upload.
                // I/O and close failures never reach this branch and can never authorize deletion.
                if(!uploads.containsKey(d.name)&&!uploads.containsValue(d.id))throw invalid;
                backend.delete(d.id,s);check(s);removedIds.add(d.id);
                for(Map.Entry<String,String> upload:uploads.entrySet())if(upload.getKey().equals(d.name)||upload.getValue().equals(d.id))completedUploads.add(upload.getKey());continue;
            }
            all.add(g);for(Map.Entry<String,String> upload:uploads.entrySet())if(upload.getKey().equals(d.name)||upload.getValue().equals(d.id))completedUploads.add(upload.getKey());
        }
        if(!removedIds.isEmpty()){
            Set<String> remaining=new HashSet<>();for(Doc dir:directories(s))for(Doc d:backend.list(dir.id,s))remaining.add(d.id);
            for(String id:removedIds)if(remaining.contains(id))throw new IOException("Failed upload deletion not confirmed");
        }
        // Planned creates which never reached the provider are harmless; retain entries not observed
        // until publication succeeds, so a late provider completion is still attributable after restart.
        for(String name:completedUploads)uploads.remove(name);saveJournal(uploads);
        Map<String,SafCloudFormat.Unit> units=SafCloudFormat.resolve(all);
        File stage=Files.createTempDirectory(s.cache.getParentFile().toPath(),".saf-stage-").toFile();
        try{
            for(Map.Entry<String,SafCloudFormat.Unit> e:units.entrySet()){
                check(s);SafCloudFormat.Unit unit=e.getValue();writeLocal(new File(stage,e.getKey()+".pack"),unit.selected.pack.bytes,s);
                for(SafCloudFormat.Generation g:unit.heads)if(!g.id.equals(unit.selected.id))writeLocal(new File(new File(new File(stage,"conflicts"),e.getKey()),g.id+".pack"),g.pack.bytes,s);
            }
            synchronized(lock){check(s);if(listFiles(s.cache).length!=0)throw new IOException("Cache changed during refresh");for(File f:listFiles(stage))Files.move(f.toPath(),new File(s.cache,f.getName()).toPath());s.units=units;s.directories=dirs;s.refreshed=true;}
        }finally{removeLocal(stage);}
    }
    private void publishWorker(Session s)throws IOException{
        check(s);Map<String,SafCloudFormat.Pack> packs=new TreeMap<>();
        for(File f:listFiles(s.cache)){
            if(f.getName().equals("conflicts")&&f.isDirectory()&&!Files.isSymbolicLink(f.toPath()))continue;
            if(!f.getName().endsWith(".pack")||!f.isFile()||Files.isSymbolicLink(f.toPath()))throw new IOException("Unexpected file in provider cache");
            String unit=f.getName().substring(0,f.getName().length()-5);SafCloudFormat.validUnit(unit);byte[] bytes;
            try(InputStream in=new FileInputStream(f)){bytes=readAll(in,SafCloudFormat.MAX_PACK_BYTES,s);}packs.put(unit,SafCloudFormat.parsePack(unit,bytes));
        }
        if(packs.size()>SafCloudFormat.MAX_UNITS||!packs.keySet().containsAll(s.units.keySet()))throw new IOException("Cache lost an existing cloud unit; deletion requires a tombstone pack");
        Map<String,String> uploads=loadJournal();List<Doc> dirs=s.directories;
        for(Map.Entry<String,SafCloudFormat.Pack> e:packs.entrySet()){
            check(s);SafCloudFormat.Unit old=s.units.get(e.getKey());if(old!=null&&old.selected.pack.hash.equals(e.getValue().hash))continue;
            if(dirs.isEmpty()){
                backend.create(backend.root(),DocumentsContract.Document.MIME_TYPE_DIR,DIRECTORY,s);dirs=directories(s);if(dirs.isEmpty())throw new IOException("Created cloud directory is not listed");
            }
            byte[] envelope=SafCloudFormat.encode(e.getKey(),old==null?Collections.emptyList():old.parentIds(),e.getValue().bytes);
            SafCloudFormat.Generation g=SafCloudFormat.decode(envelope);String name="g1-"+g.id+"-"+UUID.randomUUID().toString().replace("-","")+SUFFIX;
            uploads.put(name,"");saveJournal(uploads);check(s);
            Doc created=backend.create(dirs.get(0).id,"application/octet-stream",name,s);check(s);
            uploads.put(name,created.id);saveJournal(uploads);check(s);
            if(!name.equals(created.name))throw new IOException("Provider changed the immutable document name");
            try(OutputStream out=backend.write(created.id,s)){
                if(out==null)throw new IOException("Provider returned no writable stream");s.onCancel(()->quietClose(out));
                for(int p=0;p<envelope.length;p+=32768){check(s);out.write(envelope,p,Math.min(32768,envelope.length-p));}out.flush();
            }
            check(s);byte[] actual=readDocument(new Doc(created.id,name,false,envelope.length),s);
            SafCloudFormat.Generation verified=SafCloudFormat.decode(actual);
            if(!verified.id.equals(g.id)||!Arrays.equals(actual,envelope))throw new IOException("Provider readback differs from publication");
            check(s);uploads.remove(name);saveJournal(uploads);
        }
        check(s);
    }
    private Map<String,String> loadJournal()throws IOException{
        Map<String,String> out=new TreeMap<>();if(!journal.exists())return out;
        if(Files.isSymbolicLink(journal.toPath())||journal.length()>2*1024*1024)throw new IOException("Invalid upload journal");
        List<String> lines=Files.readAllLines(journal.toPath(),StandardCharsets.UTF_8);for(String line:lines){String[] parts=line.split("\t",-1);if(parts.length!=2||!generationName(parts[0])||out.containsKey(parts[0]))throw new IOException("Corrupt upload journal");try{out.put(parts[0],new String(Base64.getDecoder().decode(parts[1]),StandardCharsets.UTF_8));}catch(IllegalArgumentException e){throw new IOException("Corrupt upload journal",e);}}return out;
    }
    private void saveJournal(Map<String,String> entries)throws IOException{
        File dir=journal.getParentFile();if(!dir.isDirectory()&&!dir.mkdirs())throw new IOException("Cannot create upload journal directory");
        if(durable)syncDirectory(dir.getParentFile());
        File temp=new File(dir,journal.getName()+".new");
        try(FileOutputStream out=new FileOutputStream(temp)){
            for(Map.Entry<String,String> e:entries.entrySet())out.write((e.getKey()+"\t"+Base64.getEncoder().encodeToString(e.getValue().getBytes(StandardCharsets.UTF_8))+"\n").getBytes(StandardCharsets.UTF_8));out.getFD().sync();
        }
        Files.move(temp.toPath(),journal.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);if(durable)syncDirectory(dir);
    }
    private static void syncDirectory(File dir)throws IOException{
        FileDescriptor fd=null;try{fd=Os.open(dir.getPath(),OsConstants.O_RDONLY,0);Os.fsync(fd);}catch(ErrnoException e){throw new IOException("Cannot persist upload journal directory",e);}finally{if(fd!=null)try{Os.close(fd);}catch(ErrnoException e){throw new IOException("Cannot close upload journal directory",e);}}
    }
    private static File[] listFiles(File dir)throws IOException{File[] files=dir.listFiles();if(files==null)throw new IOException("Cannot list private cache");return files;}
    private static void writeLocal(File file,byte[] bytes,Session s)throws IOException{
        s.check();File dir=file.getParentFile();if(!dir.isDirectory()&&!dir.mkdirs())throw new IOException("Cannot create cache directory");
        try(FileOutputStream out=new FileOutputStream(file)){for(int p=0;p<bytes.length;p+=32768){s.check();out.write(bytes,p,Math.min(32768,bytes.length-p));}out.getFD().sync();}s.check();
    }
    private static void removeLocal(File f){if(f.isDirectory()&&!Files.isSymbolicLink(f.toPath())){File[] children=f.listFiles();if(children!=null)for(File c:children)removeLocal(c);}f.delete();}
    private static void quietClose(Closeable c){try{c.close();}catch(Exception ignored){}}

    private static final class AndroidBackend implements Backend {
        private final ContentResolver resolver;private final Uri tree;private final String root;
        AndroidBackend(ContentResolver resolver,Uri tree)throws IOException{providerId(tree);this.resolver=resolver;this.tree=tree;root=DocumentsContract.getTreeDocumentId(tree);}
        public String root(){return root;}
        private Uri uri(String id){return DocumentsContract.buildDocumentUriUsingTree(tree,id);}
        private CancellationSignal signal(Session s)throws IOException{CancellationSignal signal=new CancellationSignal();s.onCancel(signal::cancel);return signal;}
        public List<Doc> list(String parent,Session s)throws IOException{
            s.check();String[] projection={DocumentsContract.Document.COLUMN_DOCUMENT_ID,DocumentsContract.Document.COLUMN_DISPLAY_NAME,DocumentsContract.Document.COLUMN_MIME_TYPE,DocumentsContract.Document.COLUMN_SIZE,DocumentsContract.Document.COLUMN_FLAGS};
            try(Cursor c=resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree,parent),projection,null,null,null,signal(s))){
                if(c==null)throw new IOException("Provider returned no listing");if(c.getExtras().getBoolean(DocumentsContract.EXTRA_LOADING,false)||c.getExtras().getString(DocumentsContract.EXTRA_ERROR)!=null)throw new IOException("Provider listing is loading or failed");
                List<Doc> docs=new ArrayList<>();Set<String> ids=new HashSet<>();
                while(c.moveToNext()){s.check();String id=c.getString(0),name=c.getString(1);if(id==null||name==null||!ids.add(id)||docs.size()>=SafCloudFormat.MAX_GENERATIONS+256)throw new IOException("Invalid/oversized provider listing");if((c.getLong(4)&DocumentsContract.Document.FLAG_VIRTUAL_DOCUMENT)!=0)throw new IOException("Virtual documents cannot contain cloud packs");docs.add(new Doc(id,name,DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2)),c.isNull(3)?-1:c.getLong(3)));}
                if(c.getExtras().getBoolean(DocumentsContract.EXTRA_LOADING,false)||c.getExtras().getString(DocumentsContract.EXTRA_ERROR)!=null)throw new IOException("Provider listing changed to loading/error");return docs;
            }catch(SecurityException|IllegalArgumentException|android.os.OperationCanceledException e){throw new IOException("Provider listing denied/cancelled",e);}
        }
        public Doc create(String parent,String mime,String name,Session s)throws IOException{
            s.check();try{
                Uri created=DocumentsContract.createDocument(resolver,uri(parent),mime,name);if(created==null)throw new IOException("Provider did not create a document");s.check();String id=DocumentsContract.getDocumentId(created);
                String[] columns={DocumentsContract.Document.COLUMN_DISPLAY_NAME,DocumentsContract.Document.COLUMN_MIME_TYPE,DocumentsContract.Document.COLUMN_SIZE};
                try(Cursor c=resolver.query(uri(id),columns,null,null,null,signal(s))){if(c==null||!c.moveToFirst()||c.getExtras().getBoolean(DocumentsContract.EXTRA_LOADING,false)||c.getExtras().getString(DocumentsContract.EXTRA_ERROR)!=null)throw new IOException("Created document metadata is unavailable");return new Doc(id,c.getString(0),DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(1)),c.isNull(2)?-1:c.getLong(2));}
            }catch(SecurityException|IllegalArgumentException e){throw new IOException("Provider creation denied",e);}
        }
        public InputStream read(String id,Session s)throws IOException{
            s.check();try{ParcelFileDescriptor fd=resolver.openFileDescriptor(uri(id),"r",signal(s));if(fd==null)throw new IOException("Provider did not open document");return new ParcelFileDescriptor.AutoCloseInputStream(fd);}catch(SecurityException|IllegalArgumentException|android.os.OperationCanceledException e){throw new IOException("Provider read denied/cancelled",e);}
        }
        public OutputStream write(String id,Session s)throws IOException{
            s.check();try{ParcelFileDescriptor fd=resolver.openFileDescriptor(uri(id),"w",signal(s));if(fd==null)throw new IOException("Provider did not open writable document");return new ParcelFileDescriptor.AutoCloseOutputStream(fd);}catch(SecurityException|IllegalArgumentException|android.os.OperationCanceledException e){throw new IOException("Provider write denied/cancelled",e);}
        }
        public void delete(String id,Session s)throws IOException{
            s.check();try{if(!DocumentsContract.deleteDocument(resolver,uri(id)))throw new IOException("Provider did not delete failed upload");}catch(SecurityException|IllegalArgumentException e){throw new IOException("Provider cleanup denied",e);}
        }
    }
}
