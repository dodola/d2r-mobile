/* 独立 QA 应用的合成文档提供器，不编入发布包。
 * Synthetic document provider for the isolated QA app; excluded from release packages. */
package org.d2r.binaryprobe;
import android.content.*;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.*;
public final class LauncherTestProvider extends ContentProvider {
    @Override public boolean onCreate(){return true;}
    @Override public ParcelFileDescriptor openFile(Uri uri,String mode)throws FileNotFoundException {
        String name=uri.getLastPathSegment();if(name==null||!name.matches("[a-z-]+\\.zip"))throw new FileNotFoundException("Invalid test document");
        return ParcelFileDescriptor.open(new File(getContext().getFilesDir(),name),ParcelFileDescriptor.parseMode(mode));
    }
    @Override public String getType(Uri u){return "application/zip";}
    @Override public Cursor query(Uri u,String[] p,String s,String[] a,String sort){return null;}
    @Override public Uri insert(Uri u,ContentValues v){throw new UnsupportedOperationException();}
    @Override public int delete(Uri u,String s,String[] a){throw new UnsupportedOperationException();}
    @Override public int update(Uri u,ContentValues v,String s,String[] a){throw new UnsupportedOperationException();}
}
