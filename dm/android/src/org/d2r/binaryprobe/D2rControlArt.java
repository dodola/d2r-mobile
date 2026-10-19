package org.d2r.binaryprobe;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import java.util.HashMap;

/** Canvas port of iOS DmTouchControlArt.h, in the same logical point geometry. */
final class D2rControlArt {
    private final HashMap<String,Bitmap> cache=new HashMap<>();
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private final RectF rect=new RectF();
    private final float density;
    D2rControlArt(float density){this.density=density;}
    private static int rgba(double r,double g,double b,double a){return Color.argb((int)Math.round(a*255),(int)Math.round(r*255),(int)Math.round(g*255),(int)Math.round(b*255));}
    private void ellipse(Canvas c,float side,float inset,int color,float stroke){
        paint.setShader(null);paint.setColor(color);paint.setStyle(stroke>0?Paint.Style.STROKE:Paint.Style.FILL);paint.setStrokeWidth(stroke);
        c.drawCircle(side/2,side/2,side*(.5f-inset),paint);
    }
    private void gradient(Canvas c,float side,float inset,int[] colors){
        float edge=side*inset;Path clip=new Path();clip.addCircle(side/2,side/2,side/2-edge,Path.Direction.CW);
        c.save();c.clipPath(clip);paint.setStyle(Paint.Style.FILL);paint.setShader(new LinearGradient(0,edge,0,side-edge,colors,null,Shader.TileMode.CLAMP));
        c.drawRect(0,0,side,side,paint);paint.setShader(null);c.restore();
    }
    private Bitmap art(String key,float side,int type,boolean held,boolean empty,boolean primary,Bitmap socket){
        int pixels=Math.max(1,Math.round(side*density));key=key+":"+pixels;
        Bitmap image=cache.get(key);if(image!=null)return image;
        image=Bitmap.createBitmap(pixels,pixels,Bitmap.Config.ARGB_8888);Canvas c=new Canvas(image);c.scale(pixels/side,pixels/side);
        paint.reset();paint.setAntiAlias(true);paint.setFilterBitmap(true);paint.setStrokeCap(Paint.Cap.BUTT);
        if(type==0) {
            if(socket!=null&&!empty){
                rect.set(0,0,side,side);c.drawBitmap(socket,null,rect,paint);
                Path inner=new Path();inner.addCircle(side/2,side/2,side*.42f,Path.Direction.CW);c.save();c.clipPath(inner);
                ColorMatrix gray=new ColorMatrix();gray.setSaturation(0);paint.setColorFilter(new ColorMatrixColorFilter(gray));c.drawBitmap(socket,null,rect,paint);paint.setColorFilter(null);c.restore();
                if(held)ellipse(c,side,.052f,rgba(.64,.64,.64,.72),side*.015f);
            } else {
                gradient(c,side,.018f,new int[]{rgba(.26,.28,.28,empty?.22:.9),rgba(.055,.065,.07,empty?.22:.95),rgba(.16,.11,.10,empty?.16:.7)});
                ellipse(c,side,.018f,rgba(.008,.012,.015,empty?.38:.9),side*.025f);
                gradient(c,side,.055f,new int[]{rgba(.021,.021,.021,empty?.08:.94),rgba(.050,.050,.050,empty?.16:.94)});
                ellipse(c,side,.055f,empty?rgba(held?.72:.62,held?.72:.62,held?.72:.62,held?.80:.60):rgba(held?.64:.26,held?.64:.29,held?.64:.28,held?.72:.55),side*(held?.018f:empty?.012f:.009f));
                if(primary&&!empty)ellipse(c,side,.035f,rgba(.31,.23,.22,.52),side*.012f);
                if(empty){paint.setStyle(Paint.Style.STROKE);paint.setColor(rgba(.60,.65,.64,held?.75:.25));paint.setStrokeWidth(side*.018f);paint.setStrokeCap(Paint.Cap.ROUND);
                    float center=side/2,arm=side*.055f;c.drawLine(center-arm,center,center+arm,center,paint);c.drawLine(center,center-arm,center,center+arm,paint);}
            }
        } else if(type==1) {
            paint.setStyle(Paint.Style.FILL);paint.setShader(new RadialGradient(side/2,side/2,side*.48f,new int[]{rgba(.025,.025,.025,held?.10:.045),rgba(.025,.025,.025,held?.22:.12),rgba(.025,.025,.025,0)},new float[]{0,.78f,1},Shader.TileMode.CLAMP));
            c.drawCircle(side/2,side/2,side*.48f,paint);paint.setShader(null);paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(side*.004f);paint.setStrokeCap(Paint.Cap.ROUND);
            for(int i=0;i<4;i++){
                paint.setColor(rgba(.63,.63,.63,held?.12:.055));float radius=side*.416f;rect.set(side/2-radius,side/2-radius,side/2+radius,side/2+radius);c.drawArc(rect,i*90+11.459156f,67.08169f,false,paint);
                c.save();c.translate(side/2,side/2);c.rotate(i*90);float x=side*.414f,k=side*.028f;
                paint.setColor(rgba(.68,.68,.68,held?.25:.13));Path arrow=new Path();arrow.moveTo(x+k,0);arrow.lineTo(x-k,-k*.65f);arrow.lineTo(x-k*.42f,0);arrow.lineTo(x-k,k*.65f);arrow.close();c.drawPath(arrow,paint);c.drawLine(x-k*.42f,0,x-k*2.1f,0,paint);c.restore();
            }
        } else {
            gradient(c,side,.055f,new int[]{rgba(.23,.23,.23,held?.78:.46),rgba(.14,.14,.14,held?.82:.49),rgba(.065,.065,.065,held?.84:.52)});
            ellipse(c,side,.055f,rgba(.025,.025,.025,held?.65:.32),side*.035f);
            paint.setStyle(Paint.Style.STROKE);paint.setColor(rgba(.64,.64,.64,held?.40:.22));paint.setStrokeWidth(side*.014f);
            float radius=side*.437f;rect.set(side/2-radius,side/2-radius,side/2+radius,side/2+radius);c.drawArc(rect,192.6f,151.2f,false,paint);
            paint.setColor(rgba(.42,.42,.42,held?.20:.10));paint.setStrokeWidth(side*.012f);radius=side*.410f;rect.set(side/2-radius,side/2-radius,side/2+radius,side/2+radius);c.drawArc(rect,201.6f,136.8f,false,paint);
        }
        if(cache.size()>=64)cache.clear();cache.put(key,image);return image;
    }
    private void draw(Canvas c,String key,float x,float y,float radius,int type,boolean held,boolean empty,boolean primary,Bitmap socket){
        if(radius<=0)return;Bitmap bitmap=art(key,radius*2,type,held,empty,primary,socket);rect.set(x-radius,y-radius,x+radius,y+radius);
        paint.reset();paint.setAntiAlias(true);paint.setFilterBitmap(true);c.drawBitmap(bitmap,null,rect,paint);
    }
    void button(Canvas c,float x,float y,float radius,boolean held,boolean empty,boolean primary,Bitmap socket){draw(c,"button:"+held+":"+empty+":"+primary+":"+(socket!=null),x,y,radius,0,held,empty,primary,socket);}
    void joystick(Canvas c,float x,float y,float radius,float thumbX,float thumbY,boolean active){
        draw(c,"base:"+active,x,y,radius,1,active,false,false,null);
        draw(c,"thumb:"+active,thumbX,thumbY,radius*23/60,2,active,false,false,null);
    }
}
