package org.d2r.binaryprobe;

import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.Log;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.HashMap;
import java.util.Locale;

/** Draws copied HUD values and official atlas rectangles; never reads game memory. */
final class D2rHudRenderer {
    static final int WORDS=107, HEAD=28, STRIDE=12, PORTAL=88;
    private static final String ROOT="d2r_hud/", UI=ROOT+"hud_v4_ui/";
    private final AssetManager assets;
    private final HashMap<String,Bitmap> images=new HashMap<>();
    private final Bitmap[] atlases=new Bitmap[9];
    private final boolean[] atlasTried=new boolean[9];
    private final boolean[] silver=new boolean[9];
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private final Rect source=new Rect();
    private final RectF dest=new RectF();
    private final Path skillClip=new Path();
    private final Typeface captionTypeface=Typeface.create("serif",Typeface.NORMAL);
    private JSONObject ui;
    private boolean hudReported,skillReported;
    private final ColorMatrixColorFilter grayscale;
    D2rHudRenderer(AssetManager assets) {
        this.assets=assets;
        ColorMatrix gray=new ColorMatrix();gray.setSaturation(0);grayscale=new ColorMatrixColorFilter(gray);
        try(InputStream input=assets.open(UI+"ui.json")) {
            ByteArrayOutputStream bytes=new ByteArrayOutputStream();byte[] block=new byte[4096];int n;
            while((n=input.read(block))>0)bytes.write(block,0,n);
            ui=new JSONObject(new String(bytes.toByteArray(),"UTF-8"));
            if(ui.getJSONArray("ref").getInt(0)!=2868 || ui.getJSONArray("ref").getInt(1)!=1320)throw new IllegalArgumentException("HUD reference size");
            Log.i("D2rHUD","assets_loaded geometry=2868x1320 snapshot_words="+WORDS);
        } catch(Exception error) {ui=null;Log.i("D2rHUD","assets_unavailable "+error.getClass().getSimpleName());}
    }
    private Bitmap image(String name) {
        if(images.containsKey(name))return images.get(name);
        Bitmap result=null;
        try(InputStream input=assets.open(name)) {BitmapFactory.Options options=new BitmapFactory.Options();options.inScaled=false;result=BitmapFactory.decodeStream(input,null,options);}
        catch(Exception ignored) {}
        images.put(name,result);return result;
    }
    private Bitmap atlas(int index) {
        if(index<0||index>=9)return null;
        if(!atlasTried[index]) {
            atlasTried[index]=true;
            Bitmap candidate=image(UI+"sil_dm_native_skill_"+index+".png");
            silver[index]=candidate!=null;
            if(candidate==null)candidate=image(ROOT+"dm_native_skill_"+index+".png");
            int width=index==8?2720:4080;
            if(candidate!=null&&candidate.getWidth()==width&&candidate.getHeight()==65) {
                atlases[index]=candidate;Log.i("D2rHUD","atlas_loaded index="+index+" size="+width+"x65");
            } else Log.i("D2rHUD","atlas_unavailable index="+index);
        }
        return atlases[index];
    }
    static boolean valid(long[] h) {return h!=null&&((h.length==88&&h[0]==1)||(h.length==WORDS&&h[0]==2));}
    static boolean extended(long[] h){return valid(h)&&h[0]==2;}
    static boolean navigation(long[] h){return extended(h)&&h[100]!=0;}
    static boolean visible(long[] h,int key){return !extended(h)||(h[104]&(1L<<key))!=0;}
    private static int glyphAt(int key){return key==5?PORTAL:HEAD+key*STRIDE;}
    Bitmap socket(){return image(ROOT+"dm_mobile_socket.png");}
    boolean hasSkill(long[] h,int key,boolean pressed) {
        if(!valid(h)||h[2]==0||h[27]==0||h[26]==0||key<0||key>5||(key==5&&!extended(h)))return false;
        int at=glyphAt(key);if(h[at]!=2)return false;Bitmap bitmap=atlas((int)h[at+1]);
        long x=h[at+(pressed?6:2)],y=h[at+(pressed?7:3)],w=h[at+4],height=h[at+5];
        return bitmap!=null&&x>=0&&y>=0&&w>0&&height>0&&x+w<=bitmap.getWidth()&&y+height<=bitmap.getHeight();
    }
    boolean drawSkill(Canvas canvas,long[] h,int key,float x,float y,float radius,boolean pressed) {
        if(radius<=0||!hasSkill(h,key,pressed))return false;
        int at=glyphAt(key),index=(int)h[at+1];Bitmap bitmap=atlas(index);
        int sx=(int)h[at+(pressed?6:2)],sy=(int)h[at+(pressed?7:3)];
        int trim=silver[index]?0:Math.round(h[at+4]*.08f);
        source.set(sx+trim,sy+trim,sx+(int)h[at+4]-trim,sy+(int)h[at+5]-trim);
        float r=radius*.76f;dest.set(x-r,y-r,x+r,y+r);
        int kind=(int)h[at+11],charges=(int)(h[at+11]>>>32);boolean spent=(kind&2)!=0&&charges<=0;
        canvas.save();skillClip.reset();skillClip.addCircle(x,y,radius*.82f,Path.Direction.CW);canvas.clipPath(skillClip);
        if(spent){paint.setStyle(Paint.Style.FILL);paint.setColor(0xff4d4d4d);canvas.drawCircle(x,y,radius*.84f,paint);}
        paint.setColor(Color.WHITE);paint.setColorFilter(silver[index]||spent?grayscale:null);paint.setAlpha(spent?140:pressed?255:240);
        if(!spent)paint.setShadowLayer(pressed?radius*.10f:radius*.055f,0,0,0x52a6a6a6);
        canvas.drawBitmap(bitmap,source,dest,paint);paint.clearShadowLayer();paint.setColorFilter(null);paint.setAlpha(255);
        if(spent){paint.setColor(0x59000000);canvas.drawCircle(x,y,radius*.88f,paint);paint.setColor(Color.WHITE);}
        canvas.restore();
        if((kind&1)!=0){float br=radius*.30f,bx=x+radius*.74f,by=y-radius*.74f;
            paint.setStyle(Paint.Style.FILL);paint.setColor(0xf21c1814);canvas.drawCircle(bx,by,br,paint);
            paint.setStyle(Paint.Style.STROKE);paint.setColor(0xffa18045);paint.setStrokeWidth(Math.max(1,br*.14f));canvas.drawCircle(bx,by,br-Math.max(1,br*.08f),paint);
            caption(canvas,"Ⅱ",bx,by,br*1.15f,1);}
        if((kind&2)!=0&&ui!=null)try{float scale=viewportHeight/1320f;if(scale>0)text(canvas,Integer.toString(Math.max(0,charges)),ui.getJSONObject("badge").getJSONObject("glyphs"),(x+radius*.66f-viewportWidth/2)/scale+1434,(y+radius*.42f)/scale,viewportWidth,scale,true);}catch(Exception ignored){}
        if(!skillReported){skillReported=true;Log.i("D2rHUD","glyph_drawn key="+key+" raw="+h[at+8]+" atlas="+h[at+1]);}
        return true;
    }
    private float viewportWidth,viewportHeight;
    void viewport(float width,float height){viewportWidth=width;viewportHeight=height;}
    boolean empty(long[] h,int key){return valid(h)&&key>=0&&key<5&&(h[glyphAt(key)]==1||key==0&&!hasSkill(h,key,false));}
    void skillLabel(Canvas canvas,long[] h,int key,float x,float y,float radius){
        String label=key==0?"lbl_main.png":"lbl_f"+(key+(h[17]!=0?4:0))+".png";Bitmap bitmap=image(UI+label);if(bitmap==null)return;
        float height=radius*(key==0?.30f:.36f),width=bitmap.getWidth()*height/bitmap.getHeight();dest.set(x-width/2,y+radius*.50f-height/2,x+width/2,y+radius*.50f+height/2);paint.setColor(Color.WHITE);paint.setAlpha(255);canvas.drawBitmap(bitmap,null,dest,paint);
    }
    void fixed(Canvas canvas,int key,float x,float y,float radius,boolean pressed){
        Bitmap circle=image(ROOT+"dm_native_circle.png");paint.setColor(Color.WHITE);paint.setAlpha(pressed?153:71);dest.set(x-radius,y-radius,x+radius,y+radius);
        if(circle!=null)canvas.drawBitmap(circle,null,dest,paint);
        else{paint.setStyle(Paint.Style.FILL);paint.setColor(0x29050505);canvas.drawCircle(x,y,radius,paint);paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1);paint.setColor(0x47a88752);canvas.drawCircle(x,y,radius,paint);paint.setStyle(Paint.Style.FILL);}
        Bitmap icon=key==11||key==10?image(ROOT+(key==11?"dm_native_map.png":"dm_native_menu.png")):null;
        if(icon!=null){int sx=pressed?36:0;source.set(sx,0,sx+36,36);float r=radius*.62f;dest.set(x-r,y-r,x+r,y+r);paint.setColor(Color.WHITE);paint.setAlpha(pressed?255:209);canvas.drawBitmap(icon,source,dest,paint);}paint.setAlpha(255);
    }
    void caption(Canvas canvas,String text,float x,float y,float size,float alpha){paint.setShader(null);paint.setColorFilter(null);paint.setStyle(Paint.Style.FILL);paint.setColor(0xffd9c9a6);paint.setAlpha(Math.round(alpha*255));paint.setTypeface(captionTypeface);paint.setTextAlign(Paint.Align.CENTER);paint.setTextSize(size);canvas.drawText(text,x,y-(paint.ascent()+paint.descent())/2,paint);paint.setAlpha(255);}
    private float x(double value,float width,float scale) {return (float)((value-1434)*scale+width/2);}
    private boolean text(Canvas canvas,String text,JSONObject glyphs,double center,double baseline,float width,float scale) {
        return text(canvas,text,glyphs,center,baseline,width,scale,false);
    }
    private boolean text(Canvas canvas,String text,JSONObject glyphs,double center,double baseline,float width,float scale,boolean alignRight) {
        float advance=0;
        for(int i=0;i<text.length();i++) {JSONObject g=glyphs.optJSONObject(text.substring(i,i+1));if(g==null)return false;advance+=g.optDouble("adv");if(g.opt("file") instanceof String && image(UI+g.optString("file"))==null)return false;}
        float pen=(float)center-(alignRight?advance:advance/2);
        for(int i=0;i<text.length();i++) {
            JSONObject g=glyphs.optJSONObject(text.substring(i,i+1));Bitmap bitmap=g.opt("file") instanceof String?image(UI+g.optString("file")):null;
            if(bitmap!=null) {float left=x(pen+g.optDouble("dx"),width,scale),top=(float)(baseline+g.optDouble("dy"))*scale;
                dest.set(left,top,left+bitmap.getWidth()*scale,top+bitmap.getHeight()*scale);canvas.drawBitmap(bitmap,null,dest,paint);}
            pen+=g.optDouble("adv");
        }
        return true;
    }
    private static int rgb(org.json.JSONArray channels) {return Color.rgb(channels.optInt(0),channels.optInt(1),channels.optInt(2));}
    void drawHud(Canvas canvas,long[] h,float width,float height) {
        if(ui==null||!valid(h)||h[1]==0||h[2]==0)return;
        try {
            float scale=height/1320f;JSONObject vital=ui.getJSONObject("vital"),lv=ui.getJSONObject("lv"),exp=ui.getJSONObject("exp");
            paint.setStyle(Paint.Style.FILL);paint.setColor(Color.WHITE);paint.setAlpha(255);
            Bitmap full=image(UI+"base_full.png");JSONObject base=ui.getJSONObject("base");
            if(full!=null){float left=x(base.getDouble("x"),width,scale),top=(float)base.getDouble("y")*scale;dest.set(left,top,left+full.getWidth()*scale,top+full.getHeight()*scale);canvas.drawBitmap(full,null,dest,paint);}
            org.json.JSONArray slots=ui.getJSONArray("slots"),inner=ui.getJSONArray("inner");org.json.JSONArray menu=slots.getJSONArray(5);
            boolean held=(h[19]&(1L<<10))!=0,alert=h[14]>0||h[15]!=0;
            Bitmap cross=image(UI+(alert?(held?"lvl_1.png":"lvl_0.png"):(held?"lvl_d1.png":"lvl_d0.png")));
            if(cross!=null){dest.set(x(menu.getDouble(0),width,scale),(float)menu.getDouble(1)*scale,x(menu.getDouble(2),width,scale),(float)menu.getDouble(3)*scale);
                if(h[14]>0){paint.setShadowLayer(10*scale,0,0,0xf2ffd159);canvas.drawBitmap(cross,null,dest,paint);paint.clearShadowLayer();}canvas.drawBitmap(cross,null,dest,paint);}
            boolean chinese=h[18]!=0;
            String life=String.format(Locale.ROOT,chinese?"生命: %d / %d":"Life: %d / %d",h[5],h[6]);
            String mana=String.format(Locale.ROOT,chinese?"法力: %d / %d":"Mana: %d / %d",h[7],h[8]);
            boolean drawn=text(canvas,life,vital.getJSONObject("glyphs"),vital.getDouble("left_x"),vital.getDouble("baseline"),width,scale);
            drawn&=text(canvas,mana,vital.getJSONObject("glyphs"),vital.getDouble("right_x"),vital.getDouble("baseline"),width,scale);
            drawn&=text(canvas,String.format(Locale.ROOT,"Lv %d · %d%%",h[3],h[4]/10),lv.getJSONObject("glyphs"),lv.getDouble("x"),lv.getDouble("baseline"),width,scale);
            float left=x(exp.getDouble("x0"),width,scale),right=x(exp.getDouble("x1"),width,scale),top=(float)exp.getDouble("y")*scale,bar=(float)exp.getDouble("h")*scale;
            paint.setStyle(Paint.Style.FILL);paint.setColor(rgb(exp.getJSONArray("track")));canvas.drawRect(left,top,right,top+bar,paint);
            paint.setColor(rgb(exp.getJSONArray("fill")));canvas.drawRect(left+scale,top+scale,left+scale+(right-left-2*scale)*Math.max(0,Math.min(1000,h[4]))/1000f,top+bar-scale,paint);
            paint.setColor(rgb(exp.getJSONArray("track")));int ticks=exp.getInt("ticks");
            for(int i=1;i<ticks;i++){float tick=left+Math.round((right-left)/scale*i/ticks)*scale;canvas.drawRect(tick,top+scale,tick+scale,top+bar-scale,paint);}
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(scale);paint.setColor(rgb(exp.getJSONArray("edge")));canvas.drawRect(left+scale/2,top+scale/2,right-scale/2,top+bar-scale/2,paint);paint.setStyle(Paint.Style.FILL);paint.setColor(Color.WHITE);
            JSONObject badge=ui.getJSONObject("badge");
            for(int i=1;i<=4;i++)if(h[8+i]>0){org.json.JSONArray slot=slots.getJSONArray(i);text(canvas,Long.toString(h[8+i]),badge.getJSONObject("glyphs"),slot.getDouble(0)+inner.getDouble(2)+badge.getDouble("dx"),slot.getDouble(1)+inner.getDouble(3)+badge.getDouble("dy"),width,scale,true);}
            org.json.JSONArray first=slots.getJSONArray(0);double icx=(inner.getDouble(0)+inner.getDouble(2)+1)/2,icy=(inner.getDouble(1)+inner.getDouble(3)+1)/2;
            if(h[13]>=1&&h[13]<=8)digit(canvas,(int)h[13],first.getDouble(0)+icx,first.getDouble(1)+icy,255,width,scale);
            if(extended(h)&&h[101]!=0){JSONObject pick=ui.getJSONObject("pick");Bitmap tile=image(UI+"slot.png");for(int i=0;i<8;i++){
                double cx=pick.getDouble("center_x")+(i-3.5)*pick.getDouble("pitch"),cy=pick.getDouble("y");int alpha=i+1==h[13]?255:209;
                if(tile!=null){dest.set(x(cx-tile.getWidth()/2f,width,scale),(float)(cy-tile.getHeight()/2f)*scale,x(cx+tile.getWidth()/2f,width,scale),(float)(cy+tile.getHeight()/2f)*scale);paint.setAlpha(alpha);canvas.drawBitmap(tile,null,dest,paint);paint.setAlpha(255);}digit(canvas,i+1,cx,cy,alpha,width,scale);}}
            if(drawn&&!hudReported){hudReported=true;Log.i("D2rHUD","numbers_drawn level="+h[3]+" hp="+h[5]+"/"+h[6]+" mp="+h[7]+"/"+h[8]+" exp_permille="+h[4]);}
        } catch(Exception error) {Log.i("D2rHUD","draw_unavailable "+error.getClass().getSimpleName());}
    }
    private void digit(Canvas canvas,int number,double cx,double cy,int alpha,float width,float scale){Bitmap bitmap=image(UI+"diff_"+number+".png");if(bitmap==null)return;float x=x(cx,width,scale),y=(float)cy*scale;dest.set(x-bitmap.getWidth()*scale/2,y-bitmap.getHeight()*scale/2,x+bitmap.getWidth()*scale/2,y+bitmap.getHeight()*scale/2);paint.setAlpha(alpha);canvas.drawBitmap(bitmap,null,dest,paint);paint.setAlpha(255);}
}
