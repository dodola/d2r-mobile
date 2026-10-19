package org.d2r.binaryprobe;

import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.util.Log;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;

/** iOS dm_rift_hud/mapGoalSync presentation, using an independent copied ABI. */
final class D2rWorldRenderer {
    static final int WORDS=68, BYTES=1568;
    private static final String ROOT="d2r_world/";
    private final AssetManager assets;
    private final HashMap<String,Bitmap> images=new HashMap<>();
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private final Path path=new Path();
    private Typeface font;
    D2rWorldRenderer(AssetManager assets) {
        this.assets=assets;
        try {font=Typeface.createFromAsset(assets,ROOT+"BlizzardGlobal-V5_81.ttf");Log.i("D2rWorld","font=BlizzardGlobal");}
        catch(RuntimeException missing){font=Typeface.create(Typeface.DEFAULT,Typeface.BOLD);Log.i("D2rWorld","font=system-bold fallback=missing-original");}
    }
    static final class Frame {
        final long[] w=new long[WORDS];
        final String[] text=new String[4];
    }
    static Frame decode(byte[] bytes) {
        if(bytes==null||bytes.length!=BYTES)return null;
        ByteBuffer buffer=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);Frame frame=new Frame();
        for(int i=0;i<WORDS;i++)frame.w[i]=buffer.getLong();
        if(frame.w[0]!=1||(frame.w[1]&~3L)!=0||frame.w[11]<0||frame.w[11]>12)return null;
        for(int i=0;i<4;i++){int start=WORDS*8+i*256,end=start;while(end<start+256&&bytes[end]!=0)end++;frame.text[i]=new String(bytes,start,end-start,StandardCharsets.UTF_8);}
        return frame;
    }
    static final class Goal {
        float x,y,labelHeight,symbolHeight;
        boolean inside;
        int label=-1,symbol=-1; // symbol 0..6 = Tal Rasha, 7 = original grey cross.
    }
    /** Exact mapGoalSync geometry; pure values so edge/skill avoidance can be tested. */
    static Goal place(Frame frame,int index,float width,float height) {
        long[] v=frame.w;if(index<0||index>=v[11]||v[9]<=0||width<=0||height<=0)return null;
        int mode=(int)v[7];if(mode!=1&&mode!=2)return null;
        double left,top,right,bottom,ox,oy;
        if(mode==2){left=width*.04;top=height*.06;right=width*.96;bottom=height*.78;ox=v[14]/1e6*width;oy=v[15]/1e6*height;}
        else {left=v[16]/1e6*width;top=v[17]/1e6*height;right=v[18]/1e6*width;bottom=v[19]/1e6*height;ox=(left+right)/2;oy=(top+bottom)/2;double dx=(right-left)*.06,dy=(bottom-top)*.08;left+=dx;right-=dx;top+=dy;bottom-=dy;}
        int at=32+index*3,art=(int)v[at+2];boolean symbol=art>=64;
        if(!((art>=0&&art<=30)||(art>=64&&art<=70)))return null;
        double cell=v[8]/1e6*width/v[9],dx=(double)v[at]-v[12],dy=(double)v[at+1]-v[13];
        double x=ox+(dx-dy)*1.6*cell,y=oy+(dx+dy)*.8*cell;
        boolean inside=x>=left&&x<right&&y>=top&&y<bottom&&!(mode==2&&x>width*.72&&y>height*.45);
        Goal goal=new Goal();goal.inside=inside;goal.labelHeight=height*.0236f;goal.symbolHeight=height*.085f*(mode==1?.6f:1);
        if(inside&&symbol)goal.symbol=art-64;
        else {goal.label=symbol?0:art;if(inside)goal.symbol=7;}
        if(!inside) {
            double tx=x>ox?(right-ox)/(x-ox):x<ox?(left-ox)/(x-ox):1e9;
            double ty=y>oy?(bottom-oy)/(y-oy):y<oy?(top-oy)/(y-oy):1e9,t=Math.min(tx,ty);
            x=ox+(x-ox)*t;y=oy+(y-oy)*t;
            if(mode==2&&x>width*.72&&y>height*.45)y=height*.45;
        }
        if(!Double.isFinite(x)||!Double.isFinite(y))return null;
        goal.x=(float)x;goal.y=(float)y;return goal;
    }
    private Bitmap image(String name) {
        if(images.containsKey(name))return images.get(name);Bitmap bitmap=null;
        try(InputStream input=assets.open(ROOT+"mapgoal/"+name)){BitmapFactory.Options options=new BitmapFactory.Options();options.inScaled=false;bitmap=BitmapFactory.decodeStream(input,null,options);}
        catch(Exception missing){}images.put(name,bitmap);return bitmap;
    }
    private void bitmap(Canvas canvas,Bitmap bitmap,float x,float y,float width,float height){if(bitmap!=null){paint.setColor(Color.WHITE);paint.setAlpha(255);canvas.drawBitmap(bitmap,null,new android.graphics.RectF(x,y,x+width,y+height),paint);}}
    void draw(Canvas canvas,byte[] snapshot,float width,float height) {
        Frame frame=decode(snapshot);if(frame==null||frame.w[1]==0)return;
        if((frame.w[1]&2)!=0)for(int i=0;i<frame.w[11];i++) {
            Goal goal=place(frame,i,width,height);if(goal==null)continue;Bitmap label=null,mark=null;
            if(goal.label>=0){int language=(int)frame.w[25];
                if(language!=8&&language!=9)label=image("label_"+goal.label+"_en.png");
                else if(language==8&&frame.w[26]!=0)label=image("label_"+goal.label+"_old.png");
                if(label==null)label=image("label_"+goal.label+".png");}
            if(goal.symbol>=0)mark=image(goal.symbol==7?"cross.png":"sym_"+goal.symbol+".png");
            float lh=goal.labelHeight,cw=lh*1.4f,ch=cw*18/34;
            if(label!=null){float lw=lh*label.getWidth()/label.getHeight();bitmap(canvas,label,goal.x-lw/2,mark!=null?goal.y-ch/2-2-lh:goal.y-lh/2,lw,lh);}
            if(mark!=null){boolean symbol=goal.symbol!=7;float mh=symbol?goal.symbolHeight:ch,mw=symbol?mh*mark.getWidth()/mark.getHeight():cw;bitmap(canvas,mark,goal.x-mw/2,goal.y-mh/2,mw,mh);}
        }
        if((frame.w[1]&1)!=0)drawRift(canvas,frame,width,height);
    }
    private void rect(Canvas canvas,float x,float y,float width,float height,int color){paint.setStyle(Paint.Style.FILL);paint.setColor(color);paint.setAlpha(Color.alpha(color));canvas.drawRect(x,y,x+width,y+height,paint);}
    private void text(Canvas canvas,String value,float x,float y,float width,float height,float size,int color,boolean right) {
        canvas.save();canvas.clipRect(x,y,x+width,y+height);paint.setTypeface(font);paint.setTextSize(size);paint.setColor(color);paint.setAlpha(255);paint.setStyle(Paint.Style.FILL);paint.setTextAlign(right?Paint.Align.RIGHT:Paint.Align.LEFT);paint.setShadowLayer(0,1,1,0xdb000000);
        canvas.drawText(value,right?x+width:x,y-paint.ascent(),paint);paint.clearShadowLayer();canvas.restore();
    }
    private void drawRift(Canvas canvas,Frame frame,float width,float height) {
        float u=height/614,W=250*u,H=78*u,bw=Math.max(1,u);boolean guardian=frame.w[23]!=0,done=frame.w[24]!=0;
        canvas.save();canvas.translate(28*u,150*u);rect(canvas,0,0,W,H,0xaa000000);
        rect(canvas,0,0,W,bw,0xe678643c);rect(canvas,0,H-bw,W,bw,0xe678643c);rect(canvas,0,0,2*u,H,0xff967d4b);
        int gold=0xffc7b377,red=0xffc83228;
        text(canvas,frame.text[0],10*u,6*u,40*u,22*u,17*u,guardian?red:gold,false);
        text(canvas,frame.text[1],48*u,9*u,130*u,19*u,14*u,0xffd7d2c8,false);
        text(canvas,frame.text[2],W-90*u,6*u,80*u,22*u,17*u,frame.w[22]<=60&&!done?red:gold,true);
        text(canvas,frame.text[3],10*u,32*u,W-20*u,17*u,12*u,0xff969187,false);
        float tx=11*u,ty=55*u,tw=W-22*u,th=10*u;rect(canvas,tx,ty,tw,th,0xff19140e);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(bw);paint.setColor(0xff5f4e30);canvas.drawRect(tx+bw/2,ty+bw/2,tx+tw-bw/2,ty+th-bw/2,paint);
        float fw=(tw-2*bw)*(done?1:Math.max(0,Math.min(1,frame.w[21]/1e6f))),fh=th-2*bw;
        paint.setStyle(Paint.Style.FILL);paint.setShader(new LinearGradient(0,ty+bw,0,ty+bw+fh,guardian?0xffbe3228:0xffcda050,guardian?0xff6e140f:0xff8c6428,Shader.TileMode.CLAMP));canvas.drawRect(tx+bw,ty+bw,tx+bw+fw,ty+bw+fh,paint);paint.setShader(null);
        for(int i=0;i<9;i++)rect(canvas,tx+bw+(tw-2*bw)*(i+1)/10,ty+bw,bw,fh,0x6e000000);
        for(int i=0;i<2;i++){float cx=i==0?tx-4*u:tx+tw+4*u,cy=ty+th/2,r=4*u;path.reset();path.moveTo(cx,cy-r);path.lineTo(cx+r,cy);path.lineTo(cx,cy+r);path.lineTo(cx-r,cy);path.close();paint.setColor(0xffa08750);paint.setStyle(Paint.Style.FILL);canvas.drawPath(path,paint);paint.setColor(0xff3c301e);paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(Math.max(1,u*.8f));canvas.drawPath(path,paint);}
        paint.setStyle(Paint.Style.FILL);canvas.restore();
    }
}
