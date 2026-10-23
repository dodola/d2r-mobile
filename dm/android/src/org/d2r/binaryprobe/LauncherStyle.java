/* 启动器原创矢量背景和控件皮肤；基于 Android Canvas/Drawable，无游戏素材或动画计时器。
 * Original launcher vector scenery and control styling, using Android Canvas/Drawable without game assets or animation timers. */
package org.d2r.binaryprobe;

import android.content.res.ColorStateList;
import android.graphics.*;
import android.graphics.drawable.*;
import java.util.Random;

final class LauncherStyle {
    static final int GOLD=0xffcda875,IVORY=0xffeee7db,MUTED=0xffb0aaa0,INK=0xff101315;
    static final int PACKAGE=0,DISPLAY=1,SAVES=2,QUALITY=3,DRIVER=4,MOD=5,
            EXPORT=6,IMPORT=7,BACKUP=8,HELP=9,PLAY=10,CHEVRON=11,CHECK=12,CREST=13;
    private static final int[][] STATES={{-android.R.attr.state_enabled},{android.R.attr.state_pressed},{android.R.attr.state_focused},{}};
    static ColorStateList colors(int normal,int disabled){return new ColorStateList(new int[][]{{-android.R.attr.state_enabled},{}},new int[]{disabled,normal});}
    static GradientDrawable surface(int top,int bottom,int stroke,float radius){
        GradientDrawable d=new GradientDrawable(GradientDrawable.Orientation.TL_BR,new int[]{top,bottom});
        d.setCornerRadius(radius);if(stroke!=0)d.setStroke(1,stroke);return d;
    }
    static Drawable control(boolean primary,float radius){
        StateListDrawable states=new StateListDrawable();
        states.addState(STATES[0],surface(0xff282a2b,0xff222425,0xff41403c,radius));
        states.addState(STATES[1],surface(primary?0xfff0d8ad:0xff3b3630,primary?0xffc09a60:0xff2c2924,0xffefd0a0,radius));
        states.addState(STATES[2],surface(primary?0xffe5c796:0xff302c27,primary?0xffbb9257:0xff232524,0xffffd99b,radius));
        states.addState(STATES[3],surface(primary?0xffdfc18f:0xdf26292b,primary?0xffb38b52:0xef191d20,primary?0xffebc995:0xff65533b,radius));
        return new RippleDrawable(ColorStateList.valueOf(primary?0x33ffffff:0x33d6b582),states,surface(-1,-1,0,radius));
    }
    static Drawable row(float radius){
        StateListDrawable states=new StateListDrawable();
        states.addState(new int[]{android.R.attr.state_focused},surface(0xe52c2d2a,0xe51d2324,0xffcda875,radius));
        states.addState(new int[]{},surface(0x9f11171b,0xaf151a1d,0x3f938168,radius));
        return new RippleDrawable(ColorStateList.valueOf(0x30cda875),states,surface(-1,-1,0,radius));
    }
    private abstract static class Art extends Drawable {
        final Paint p=new Paint(Paint.ANTI_ALIAS_FLAG);int alpha=255;
        @Override public final void draw(Canvas c){int saved=c.save();Rect b=getBounds();c.translate(b.left,b.top);
            if(alpha<255)c.saveLayerAlpha(0,0,b.width(),b.height(),alpha);drawArt(c);c.restoreToCount(saved);}
        abstract void drawArt(Canvas c);
        @Override public void setAlpha(int value){alpha=value;invalidateSelf();}
        @Override public void setColorFilter(ColorFilter value){p.setColorFilter(value);invalidateSelf();}
        @Override public int getOpacity(){return PixelFormat.TRANSLUCENT;}
    }
    static final class Glyph extends Art {
        private final int kind,size;private final ColorStateList colors;
        Glyph(int kind,int size,ColorStateList colors){this.kind=kind;this.size=size;this.colors=colors;}
        @Override public int getIntrinsicWidth(){return size;}
        @Override public int getIntrinsicHeight(){return size;}
        @Override public boolean isStateful(){return colors.isStateful();}
        @Override protected boolean onStateChange(int[] state){invalidateSelf();return true;}
        private void line(Canvas c,float... xy){Path path=new Path();path.moveTo(xy[0],xy[1]);for(int i=2;i<xy.length;i+=2)path.lineTo(xy[i],xy[i+1]);c.drawPath(path,p);}
        @Override void drawArt(Canvas c){float scale=Math.min(getBounds().width(),getBounds().height())/24f;
            c.translate((getBounds().width()-24*scale)/2,(getBounds().height()-24*scale)/2);c.scale(scale,scale);
            p.setStyle(Paint.Style.STROKE);p.setStrokeWidth(1.55f);p.setStrokeCap(Paint.Cap.ROUND);p.setStrokeJoin(Paint.Join.ROUND);p.setColor(colors.getColorForState(getState(),colors.getDefaultColor()));
            switch(kind){
                case PACKAGE:line(c,3,7,12,3,21,7,21,17,12,21,3,17,3,7,12,11,21,7);line(c,12,11,12,21);line(c,7.5f,5,16.5f,9);break;
                case DISPLAY:c.drawRoundRect(3,4,21,16,2,2,p);line(c,12,16,12,20);line(c,8,20,16,20);break;
                case SAVES:c.drawRoundRect(4,4,20,21,2,2,p);line(c,8,4,8,10,16,10,16,4);line(c,8,21,8,15,16,15,16,21);break;
                case QUALITY:line(c,3,7,12,2,21,7,12,12,3,7);line(c,3,12,12,17,21,12);line(c,3,17,12,22,21,17);break;
                case DRIVER:c.drawRoundRect(6,6,18,18,2,2,p);c.drawRect(9,9,15,15,p);for(int i=8;i<=16;i+=4){line(c,i,3,i,6);line(c,i,18,i,21);line(c,3,i,6,i);line(c,18,i,21,i);}break;
                case MOD:line(c,12,2,21,7,21,17,12,22,3,17,3,7,12,2);line(c,3,7,12,12,21,7);line(c,12,12,12,22);break;
                case EXPORT:case IMPORT:line(c,4,14,4,20,20,20,20,14);if(kind==EXPORT){line(c,12,15,12,3);line(c,7,8,12,3,17,8);}else{line(c,12,3,12,15);line(c,7,10,12,15,17,10);}break;
                case BACKUP:c.drawArc(4,4,21,21,210,305,false,p);line(c,3,4,3,10,9,10);line(c,12,8,12,13,16,15);break;
                case HELP:c.drawCircle(12,12,9,p);line(c,12,11,12,17);c.drawCircle(12,7,0.5f,p);break;
                case PLAY:line(c,8,4,20,12,8,20,8,4);break;
                case CHEVRON:line(c,9,6,15,12,9,18);break;
                case CHECK:line(c,4,12,9,17,20,6);break;
                case CREST:line(c,12,1,22,12,12,23,2,12,12,1);line(c,12,5,18,12,12,19,6,12,12,5);line(c,12,8,12,16);break;
            }
        }
    }
    static final class Backdrop extends Art {
        private final float[] dust=new float[240];private int width,height;private Shader sky,glow,cold,vignette;
        Backdrop(){Random random=new Random(94012);for(int i=0;i<dust.length;i++)dust[i]=random.nextFloat();}
        private void configure(int w,int h){if(width==w&&height==h)return;width=w;height=h;
            sky=new LinearGradient(0,0,0,h,new int[]{0xff131b23,0xff1a1c20,0xff221a16},null,Shader.TileMode.CLAMP);
            glow=new RadialGradient(w*.09f,h*.83f,Math.max(w,h)*.65f,new int[]{0x826b4224,0x184b3526,0x004b3526},null,Shader.TileMode.CLAMP);
            cold=new RadialGradient(w*.83f,h*.15f,Math.max(w,h)*.60f,new int[]{0x683c5360,0x003c5360},null,Shader.TileMode.CLAMP);
            vignette=new RadialGradient(w*.5f,h*.42f,Math.max(w,h)*.8f,new int[]{0x00101418,0x10101418,0xb5080b0f},new float[]{0,.4f,1},Shader.TileMode.CLAMP);
        }
        private void fill(Canvas c,Shader shader){p.setStyle(Paint.Style.FILL);p.setColor(Color.WHITE);p.setShader(shader);c.drawRect(0,0,width,height,p);p.setShader(null);}
        private void arch(Canvas c,float x,float y,float scale){int save=c.save();c.translate(x,y);c.scale(scale,scale);
            Path frame=new Path();frame.setFillType(Path.FillType.EVEN_ODD);
            frame.moveTo(-360,1100);frame.lineTo(-360,80);frame.cubicTo(-360,-170,-110,-320,0,-470);frame.cubicTo(110,-320,360,-170,360,80);frame.lineTo(360,1100);frame.close();
            frame.moveTo(-280,1100);frame.lineTo(-280,100);frame.cubicTo(-280,-100,-70,-230,0,-360);frame.cubicTo(70,-230,280,-100,280,100);frame.lineTo(280,1100);frame.close();
            p.setColor(0x9f080e14);p.setStyle(Paint.Style.FILL);c.drawPath(frame,p);
            p.setColor(0x3a82918a);p.setStyle(Paint.Style.STROKE);p.setStrokeWidth(1.5f);c.drawPath(frame,p);
            p.setColor(0x247c877f);for(int yLine=140;yLine<1100;yLine+=100){c.drawLine(-360,yLine,-280,yLine,p);c.drawLine(280,yLine,360,yLine,p);}
            p.setColor(0x188facb2);c.drawLine(-239,240,-239,1100,p);c.drawLine(239,240,239,1100,p);
            p.setStyle(Paint.Style.FILL);p.setColor(0x3f080d12);c.drawRect(-560,430,-420,1100,p);c.drawRect(420,300,550,1100,p);c.restoreToCount(save);
        }
        @Override void drawArt(Canvas c){int w=getBounds().width(),h=getBounds().height();if(w<=0||h<=0)return;configure(w,h);
            fill(c,sky);fill(c,cold);float scale=Math.max(w/1700f,h/1700f);
            arch(c,w*.81f,h*.37f,scale);arch(c,w*.13f,h*.47f,scale*.57f);fill(c,glow);
            p.setStyle(Paint.Style.FILL);for(int i=0;i<dust.length;i+=4){p.setColor(((int)(12+dust[i+2]*42)<<24)|0xd5a66a);c.drawCircle(dust[i]*w,dust[i+1]*h,Math.max(.6f,dust[i+3]*1.5f*scale),p);}
            fill(c,vignette);
        }
    }
}
