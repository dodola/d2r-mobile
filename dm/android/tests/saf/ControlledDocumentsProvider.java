package org.d2r.binaryprobe;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsProvider;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.Arrays;

/** Disposable provider in a separate test package. No game package or save access. */
public final class ControlledDocumentsProvider extends DocumentsProvider {
    static final String AUTHORITY="org.d2r.saf.controlled.documents";
    static volatile String fault="";
    static volatile int renameCalls,queryCalls,readCalls,writeCalls,deleteCalls;
    private File base;
    private static final String[] COLUMNS={DocumentsContract.Document.COLUMN_DOCUMENT_ID,DocumentsContract.Document.COLUMN_DISPLAY_NAME,DocumentsContract.Document.COLUMN_MIME_TYPE,DocumentsContract.Document.COLUMN_SIZE,DocumentsContract.Document.COLUMN_FLAGS};
    @Override public boolean onCreate(){base=new File(getContext().getFilesDir(),"controlled-documents");new File(base,"one").mkdirs();new File(base,"two").mkdirs();return true;}
    private File file(String id)throws FileNotFoundException{
        try{File f=new File(base,id).getCanonicalFile();if(!f.getPath().startsWith(base.getCanonicalPath()+File.separator))throw new IOException("outside test provider");return f;}
        catch(IOException e){throw new FileNotFoundException(e.toString());}
    }
    private void denied(){if(fault.equals("revoked"))throw new SecurityException("controlled revoked grant");}
    private void row(MatrixCursor cursor,File f,String id){
        MatrixCursor.RowBuilder row=cursor.newRow();for(String c:cursor.getColumnNames()){
            switch(c){case DocumentsContract.Document.COLUMN_DOCUMENT_ID:row.add(id);break;
                case DocumentsContract.Document.COLUMN_DISPLAY_NAME:row.add(f.getName());break;
                case DocumentsContract.Document.COLUMN_MIME_TYPE:row.add(f.isDirectory()?DocumentsContract.Document.MIME_TYPE_DIR:"application/octet-stream");break;
                case DocumentsContract.Document.COLUMN_SIZE:row.add(f.length());break;
                case DocumentsContract.Document.COLUMN_FLAGS:row.add(f.isDirectory()?DocumentsContract.Document.FLAG_DIR_SUPPORTS_CREATE:DocumentsContract.Document.FLAG_SUPPORTS_WRITE|DocumentsContract.Document.FLAG_SUPPORTS_DELETE);break;
                default:row.add(null);}
        }
    }
    @Override public Cursor queryRoots(String[] projection){
        String[] cols=projection==null?new String[]{DocumentsContract.Root.COLUMN_ROOT_ID,DocumentsContract.Root.COLUMN_DOCUMENT_ID,DocumentsContract.Root.COLUMN_TITLE,DocumentsContract.Root.COLUMN_FLAGS}:projection;MatrixCursor out=new MatrixCursor(cols);
        for(String id:new String[]{"one","two"}){MatrixCursor.RowBuilder row=out.newRow();for(String col:cols){switch(col){case DocumentsContract.Root.COLUMN_ROOT_ID:case DocumentsContract.Root.COLUMN_DOCUMENT_ID:row.add(id);break;case DocumentsContract.Root.COLUMN_TITLE:row.add("Controlled SAF "+id);break;case DocumentsContract.Root.COLUMN_FLAGS:row.add(DocumentsContract.Root.FLAG_SUPPORTS_CREATE);break;default:row.add(null);}}}return out;
    }
    @Override public Cursor queryDocument(String id,String[] projection)throws FileNotFoundException{denied();MatrixCursor out=new MatrixCursor(projection==null?COLUMNS:projection);File f=file(id);if(!f.exists())throw new FileNotFoundException(id);row(out,f,id);return out;}
    @Override public Cursor queryChildDocuments(String parent,String[] projection,String order)throws FileNotFoundException{
        denied();queryCalls++;if(fault.equals("delay")){long until=System.nanoTime()+500000000L;while(System.nanoTime()<until)try{Thread.sleep(10);}catch(InterruptedException ignored){}}
        MatrixCursor out=new MatrixCursor(projection==null?COLUMNS:projection);File[] files=file(parent).listFiles();if(files==null)throw new FileNotFoundException(parent);Arrays.sort(files);
        for(File f:files)row(out,f,parent+"/"+f.getName());Bundle extras=new Bundle();if(fault.equals("loading"))extras.putBoolean(DocumentsContract.EXTRA_LOADING,true);if(fault.equals("error"))extras.putString(DocumentsContract.EXTRA_ERROR,"controlled query failure");out.setExtras(extras);return out;
    }
    @Override public boolean isChildDocument(String parent,String child){return child.startsWith(parent+"/");}
    @Override public String createDocument(String parent,String mime,String name)throws FileNotFoundException{
        denied();File f=file(parent+"/"+name);try{boolean ok=mime.equals(DocumentsContract.Document.MIME_TYPE_DIR)?f.mkdir():f.createNewFile();if(!ok)throw new IOException("create collision");return parent+"/"+name;}catch(IOException e){throw new FileNotFoundException(e.toString());}
    }
    @Override public String renameDocument(String id,String name)throws FileNotFoundException{renameCalls++;throw new FileNotFoundException("rename deliberately unsupported");}
    @Override public void deleteDocument(String id)throws FileNotFoundException{denied();if(fault.equals("deletefail"))throw new FileNotFoundException("controlled delete failure");if(!file(id).delete())throw new FileNotFoundException("delete failed");deleteCalls++;}
    @Override public ParcelFileDescriptor openDocument(String id,String mode,CancellationSignal signal)throws FileNotFoundException{
        denied();if(signal!=null)signal.throwIfCanceled();File f=file(id);boolean write=mode.indexOf('w')>=0;
        if(write){writeCalls++;if(fault.equals("writeopen"))throw new FileNotFoundException("controlled write open failure");return ParcelFileDescriptor.open(f,ParcelFileDescriptor.MODE_WRITE_ONLY|ParcelFileDescriptor.MODE_TRUNCATE);}
        readCalls++;if(!fault.equals("truncated")&&!fault.equals("streamerror"))return ParcelFileDescriptor.open(f,ParcelFileDescriptor.MODE_READ_ONLY);
        try{
            byte[] full=java.nio.file.Files.readAllBytes(f.toPath());final boolean error=fault.equals("streamerror");ParcelFileDescriptor[] pipe=ParcelFileDescriptor.createReliablePipe();
            Thread writer=new Thread(()->{try{OutputStream out=new ParcelFileDescriptor.AutoCloseOutputStream(pipe[1]);out.write(full,0,error?full.length:Math.max(0,full.length-1));out.flush();if(error)pipe[1].closeWithError("controlled reliable stream terminal error");else out.close();}catch(IOException ignored){try{pipe[1].closeWithError("controlled stream writer failure");}catch(IOException ignoredAgain){}}},"controlled-saf-stream");writer.setDaemon(true);writer.start();return pipe[0];
        }catch(IOException e){throw new FileNotFoundException(e.toString());}
    }
}
