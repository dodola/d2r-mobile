package org.d2r.binaryprobe;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.accessibility.AccessibilityEvent;
import java.util.function.BooleanSupplier;

/** Android rendering/events only; hit testing and gestures belong to upstream C. */
public final class TouchControlsView extends View {
    private static native void nativeResize(float width,float height,float left,float right);
    private static native void nativeEnable(boolean active);
    private static native void nativeCancel();
    private static native int nativeEvent(int[] ids,float[] xs,float[] ys,boolean cancelled);
    private static native int nativeRelease(int id,float x,float y);
    private static native float[] nativeFrame();
    private static native long[] nativeHudFrame();
    private static native byte[] nativeWorldFrame();
    private static native void nativeHudRendered(long[] frame,int drawnMask);
    private static native void nativeActivate(int target);
    private static native void nativeGameState(int navigation,int panelBits,int beltRows,int beltOpen,
                                               int holding,int pointerItem,int tapBox,int curStack,int hoverUnid,int targetMask);
    private static native int nativeHit(float x,float y);
    // Canonical Android touch-layer tokens, mirrored in DESIGN.md.
    static final int INK=0x8030241b, RIM=0xbba08359, TEXT=0xffe9d9ba, PRESSED=0xcc80623c;
    private static final String[] NAV_ZH={"确认／交换","返回","操作","绑定","下一页"},NAV_EN={"Confirm / Swap","Back","Action","Bind","Next Page"};
    private static final long[] SKILL_BUTTONS={1,2,4,8,128};
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final BooleanSupplier ready;
    private final GameUiStrings strings;
    private final D2rHudRenderer hudRenderer;
    private final D2rControlArt controlArt;
    private final D2rWorldRenderer worldRenderer;
    private boolean worldNative=true;
    private boolean hudNative=true;
    private float[] frame;
    private long[] hudFrame;
    private boolean active;
    private BooleanSupplier touchActivation;
    private int accessibilityFocus=View.NO_ID;
    private final float density;
    public TouchControlsView(Context context,BooleanSupplier ready,GameUiStrings strings){super(context);this.ready=ready;this.strings=strings;density=getResources().getDisplayMetrics().density;hudRenderer=new D2rHudRenderer(context.getAssets());controlArt=new D2rControlArt(density);worldRenderer=new D2rWorldRenderer(context.getAssets());
        updateLanguageDescription();setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);setVisibility(VISIBLE);}
    private void updateLanguageDescription(){setContentDescription(strings.text("触摸操控：左侧摇杆移动，右侧技能按钮拖动瞄准，底部药水"));}
    /** Called after the first presented frame; JNI only copies published state. */
    public boolean refreshLanguage(){
        if(!hudNative||!ready.getAsBoolean())return false;
        try{if(strings.updateHudLanguage(nativeHudFrame())){updateLanguageDescription();postInvalidateOnAnimation();return true;}}
        catch(UnsatisfiedLinkError unavailable){hudNative=false;}
        return false;
    }
    public void setActive(boolean on){if(active==on)return;active=on;nativeEnable(on);if(!on)clearHud();if(on)resize();postInvalidateOnAnimation();}
    public void setTouchActivation(BooleanSupplier activation){touchActivation=activation;}
    private void clearHud(){if(hudNative)try{nativeHudRendered(null,0);}catch(UnsatisfiedLinkError unavailable){hudNative=false;}}
    public void cancel(){nativeCancel();clearHud();}
    @Override protected void onDetachedFromWindow(){clearHud();super.onDetachedFromWindow();}
    /** Publish game/UI observations used by the portable iOS-compatible gesture state machine. */
    public void setGameState(boolean navigation,int panelBits,int beltRows,boolean beltOpen,
                             boolean holding,int pointerItem,boolean tapBox,int curStack,int hoverUnid,int targetMask){
        nativeGameState(navigation?1:0,panelBits,beltRows,beltOpen?1:0,holding?1:0,pointerItem,
            tapBox?1:0,curStack,hoverUnid,targetMask);postInvalidateOnAnimation();
    }
    /** Return the iOS-compatible control token at normalized view coordinates. */
    public int hitTarget(float x,float y){return nativeHit(Math.max(0,Math.min(.999999f,x)),Math.max(0,Math.min(.999999f,y)));}
    private void resize(){if(getWidth()==0||getHeight()==0)return;float left=0,right=0;WindowInsets insets=getRootWindowInsets();
        if(insets!=null&&insets.getDisplayCutout()!=null){
            int[] location=new int[2];getLocationOnScreen(location);
            android.graphics.Point screen=new android.graphics.Point();
            ((android.view.WindowManager)getContext().getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay().getRealSize(screen);
            left=Math.max(0,insets.getDisplayCutout().getSafeInsetLeft()-location[0])/density;
            right=Math.max(0,location[0]+getWidth()-(screen.x-insets.getDisplayCutout().getSafeInsetRight()))/density;
        }
        nativeResize(getWidth()/density,getHeight()/density,left,right);frame=nativeFrame();}
    @Override protected void onLayout(boolean changed,int left,int top,int right,int bottom){
        super.onLayout(changed,left,top,right,bottom);if(changed)resize();
    }
    private void circle(Canvas canvas,float x,float y,float radius,String label,boolean pressed){if(radius<=0)return;
        paint.setStyle(Paint.Style.FILL);paint.setColor(pressed?PRESSED:INK);canvas.drawCircle(x,y,radius,paint);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.4f);paint.setColor(RIM);canvas.drawCircle(x,y,radius,paint);
        if(label!=null){paint.setStyle(Paint.Style.FILL);paint.setColor(TEXT);paint.setTextAlign(Paint.Align.CENTER);paint.setTextSize(Math.min(14,radius*.5f));canvas.drawText(label,x,y-(paint.ascent()+paint.descent())/2,paint);}}
    private void drawWorld(Canvas canvas){if(!worldNative||!ready.getAsBoolean())return;try{byte[] world=nativeWorldFrame();canvas.save();canvas.scale(density,density);worldRenderer.draw(canvas,world,getWidth()/density,getHeight()/density);canvas.restore();}catch(UnsatisfiedLinkError unavailable){worldNative=false;}}
    @Override protected void onDraw(Canvas canvas){super.onDraw(canvas);if(!active){drawWorld(canvas);if(ready.getAsBoolean())postInvalidateOnAnimation();return;}frame=nativeFrame();if(frame==null||frame.length!=58)return;
        long[] hud=null;if(hudNative)try{hud=nativeHudFrame();}catch(UnsatisfiedLinkError unavailable){hudNative=false;}hudFrame=hud;
        int drawnMask=0;
        canvas.save();canvas.scale(density,density);
        if(D2rHudRenderer.valid(hud))drawnMask=drawD2r(canvas,hud);
        else {
        for(int i=0;i<12;i++) {boolean pressed=frame[44+i]>0;boolean glyph=i<5&&hudRenderer.hasSkill(hud,i,pressed);
            String label=glyph?null:strings.controlLabel(i,false,false);if(i<5&&D2rHudRenderer.valid(hud)&&hud[28+i*12]==1)label=null;
            circle(canvas,frame[i*3],frame[i*3+1],frame[i*3+2],label,pressed);
            if(glyph&&hudRenderer.drawSkill(canvas,hud,i,frame[i*3],frame[i*3+1],frame[i*3+2],pressed))drawnMask|=1<<i;
        }
        hudRenderer.drawHud(canvas,hud,getWidth()/density,getHeight()/density);
        circle(canvas,frame[36],frame[37],frame[38],null,frame[57]>0);circle(canvas,frame[42],frame[43],frame[38]*.38f,null,frame[57]>0);
        circle(canvas,frame[39],frame[40],frame[41],frame[56]>0?"Ⅱ":"Ⅰ",false);
        }
        if(accessibilityFocus!=View.NO_ID&&accessible(accessibilityFocus)){float x,y,r;if(accessibilityFocus<12){x=frame[accessibilityFocus*3];y=frame[accessibilityFocus*3+1];r=frame[accessibilityFocus*3+2];}
            else if(accessibilityFocus==12){x=frame[39];y=frame[40];r=frame[41];}else{x=frame[36];y=frame[37];r=frame[38];}
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(3);paint.setColor(TEXT);canvas.drawCircle(x,y,r+3,paint);}
        canvas.restore();
        drawWorld(canvas);
        if(hudNative)nativeHudRendered(hud,drawnMask);
        if(ready.getAsBoolean())postInvalidateOnAnimation();}
    private int drawD2r(Canvas canvas,long[] hud){
        if(hud[27]==0)return 0;
        int drawn=0;boolean navigation=D2rHudRenderer.navigation(hud),chinese=hud[18]!=0;
        float k=frame[38]/60;hudRenderer.viewport(getWidth()/density,getHeight()/density);
        for(int i=0;i<12;i++){
            // iOS drawRect keeps belt and cross hit areas, but their artwork belongs to drawV4.
            boolean cancel=!navigation&&i==11&&D2rHudRenderer.extended(hud)&&hud[102]!=0;
            // 隐藏地图键后，拖技能进入右上角仍显示取消提示。
            // Keep the skill-drag cancellation cue even while the map key is hidden.
            if((!D2rHudRenderer.visible(hud,i)&&!cancel)||(i>5&&i<=10))continue;
            float x=frame[i*3],y=frame[i*3+1],r=frame[i*3+2];if(r<=0)continue;
            boolean pressed=frame[44+i]>0||(i<5&&(hud[19]&SKILL_BUTTONS[i])!=0);
            if(!navigation&&i<=5){
                boolean glyph=hudRenderer.hasSkill(hud,i,pressed),empty=hudRenderer.empty(hud,i);
                controlArt.button(canvas,x,y,r,pressed,empty,i==0,hudRenderer.socket());
                if(glyph){if(hudRenderer.drawSkill(canvas,hud,i,x,y,r,pressed)&&i<5)drawn|=1<<i;}
                else if(!empty&&i<5)hudRenderer.skillLabel(canvas,hud,i,x,y,r);
            }else{
                if(cancel){paint.setStyle(Paint.Style.FILL);paint.setColor(0x5c8c0d0a);canvas.drawRect(x-r,y-r,x+r,y+r,paint);}
                hudRenderer.fixed(canvas,navigation||cancel?-1:i,x,y,r,pressed);
                if(cancel)hudRenderer.caption(canvas,chinese?"取消":"Cancel",x,y,10*k,pressed?1:.84f);
                else if(navigation&&i<5)hudRenderer.caption(canvas,chinese?NAV_ZH[i]:NAV_EN[i],x,y,(i==0?13:10)*k,pressed?1:.84f);
            }
        }
        if(!navigation&&frame[41]>0){boolean held=D2rHudRenderer.extended(hud)&&hud[103]!=0;
            controlArt.button(canvas,frame[39],frame[40],frame[41],held,false,false,hudRenderer.socket());hudRenderer.caption(canvas,frame[56]>0?"Ⅱ":"Ⅰ",frame[39],frame[40],12*k,held?1:.86f);}
        hudRenderer.drawHud(canvas,hud,getWidth()/density,getHeight()/density);
        if(!navigation&&hud[16]==0){boolean moving=frame[57]>0;float x=frame[36],y=frame[37];
            if(moving&&D2rHudRenderer.extended(hud)){x=hud[105]/1000f;y=hud[106]/1000f;}
            controlArt.joystick(canvas,x,y,frame[38],x+frame[42]-frame[36],y+frame[43]-frame[37],moving);}
        return drawn;
    }
    private boolean accessible(int id){
        if(!D2rHudRenderer.valid(hudFrame))return true;
        if(hudFrame[27]==0)return false;
        if(id<12)return D2rHudRenderer.visible(hudFrame,id);
        if(id==12)return !D2rHudRenderer.navigation(hudFrame);
        return !D2rHudRenderer.navigation(hudFrame)&&hudFrame[16]==0;
    }
    @Override public boolean onTouchEvent(MotionEvent event){
        // 实体手柄使用后，首个触摸手势立即接管并继续传递。
        // After physical-controller input, the first touch takes over and continues through this gesture.
        if(!active&&(event.getActionMasked()!=MotionEvent.ACTION_DOWN||touchActivation==null||!touchActivation.getAsBoolean()))return false;
        if(!ready.getAsBoolean())return true;
        int action=event.getActionMasked(),skip=action==MotionEvent.ACTION_POINTER_UP?event.getActionIndex():-1;
        if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP){int lifted=event.getActionIndex();
            nativeRelease(event.getPointerId(lifted),Math.max(0,Math.min(.999999f,event.getX(lifted)/getWidth())),
                Math.max(0,Math.min(.999999f,event.getY(lifted)/getHeight())));}
        int count=(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL)?0:event.getPointerCount()-(skip>=0?1:0);
        if(count>16){nativeCancel();return true;}
        int[] ids=new int[count];float[] xs=new float[count],ys=new float[count];int k=0;
        for(int i=0;i<event.getPointerCount()&&k<count;i++)if(i!=skip){ids[k]=event.getPointerId(i);xs[k]=Math.max(0,Math.min(.999999f,event.getX(i)/getWidth()));ys[k]=Math.max(0,Math.min(.999999f,event.getY(i)/getHeight()));k++;}
        nativeEvent(ids,xs,ys,action==MotionEvent.ACTION_CANCEL);postInvalidateOnAnimation();return true;}
    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider(){return new AccessibilityNodeProvider(){
        @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int id){AccessibilityNodeInfo node=AccessibilityNodeInfo.obtain();node.setPackageName(getContext().getPackageName());node.setSource(TouchControlsView.this,id);
            if(id==View.NO_ID){onInitializeAccessibilityNodeInfo(node);if(frame!=null){for(int i=0;i<12;i++)if(frame[i*3+2]>0&&accessible(i))node.addChild(TouchControlsView.this,i);if(accessible(12))node.addChild(TouchControlsView.this,12);for(int i=20;i<24;i++)if(accessible(i))node.addChild(TouchControlsView.this,i);}return node;}
            if(frame==null||!accessible(id))return null;float x,y,r;String label;
            if(id>=0&&id<12){x=frame[id*3];y=frame[id*3+1];r=frame[id*3+2];label=strings.controlLabel(id,D2rHudRenderer.valid(hudFrame)&&D2rHudRenderer.navigation(hudFrame),D2rHudRenderer.extended(hudFrame)&&hudFrame[102]!=0);}
            else if(id==12){x=frame[39];y=frame[40];r=frame[41];label=strings.controlLabel(12,false,false);}
            else if(id>=20&&id<24){r=frame[38]/3;x=frame[36]+(id==20?-r*2:id==21?r*2:0);y=frame[37]+(id==22?-r*2:id==23?r*2:0);label=strings.controlLabel(id,false,false);}
            else return null;
            node.setClassName("android.widget.Button");node.setContentDescription(label);node.setClickable(true);node.setFocusable(true);node.setAccessibilityFocused(accessibilityFocus==id);node.setEnabled(ready.getAsBoolean());node.setVisibleToUser(active);node.setParent(TouchControlsView.this);
            Rect bounds=new Rect((int)((x-r)*density),(int)((y-r)*density),(int)((x+r)*density),(int)((y+r)*density));node.setBoundsInParent(bounds);int[] location=new int[2];getLocationOnScreen(location);bounds.offset(location[0],location[1]);node.setBoundsInScreen(bounds);node.addAction(AccessibilityNodeInfo.ACTION_CLICK);node.addAction(accessibilityFocus==id?AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS:AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS);return node;}
        @Override public boolean performAction(int id,int action,Bundle args){if(!((id>=0&&id<=12)||(id>=20&&id<=23)))return false;
            if(action==AccessibilityNodeInfo.ACTION_CLICK&&active&&ready.getAsBoolean()&&accessible(id)){nativeActivate(id);postInvalidateOnAnimation();return true;}
            if(action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS||action==AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS){accessibilityFocus=action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS?id:View.NO_ID;
                AccessibilityEvent event=AccessibilityEvent.obtain(action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS?AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED:AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);event.setSource(TouchControlsView.this,id);event.setPackageName(getContext().getPackageName());event.setClassName("android.widget.Button");if(getParent()!=null)getParent().requestSendAccessibilityEvent(TouchControlsView.this,event);invalidate();return true;}
            return false;}
    };}
}
