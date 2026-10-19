package org.d2r.binaryprobe;

import android.content.Context;
import android.util.AtomicFile;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

final class RenderQualityStore {
    private final AtomicFile file;
    RenderQualityStore(Context context){file=new AtomicFile(new File(context.getFilesDir(),"d2r-mobile/diagnostics/render-scale.txt"));}
    float read()throws IOException {
        if(!file.getBaseFile().exists()&&!new File(file.getBaseFile()+".bak").exists())return RenderQuality.AUTO;
        try(FileInputStream in=file.openRead()){
            ByteArrayOutputStream data=new ByteArrayOutputStream();
            for(int b;(b=in.read())!=-1;){if(data.size()==63)throw new IOException("画质设置过长");data.write(b);}
            try{return RenderQuality.parse(new String(data.toByteArray(),StandardCharsets.US_ASCII));}
            catch(IllegalArgumentException e){throw new IOException("画质设置无效",e);}
        }
    }
    void write(float scale)throws IOException {
        byte[] data=RenderQuality.encode(scale).getBytes(StandardCharsets.US_ASCII);
        FileOutputStream out=null;
        try{out=file.startWrite();out.write(data);out.getFD().sync();file.finishWrite(out);out=null;
            // AtomicFile logs some commit failures instead of throwing. Never
            // apply a mode that was not actually persisted for the next start.
            if(read()!=scale)throw new IOException("画质设置提交失败");
        }
        catch(IOException e){if(out!=null)file.failWrite(out);throw e;}
    }
}
