package org.d2r.binaryprobe;

import android.content.Context;
import android.view.View;
import android.widget.FrameLayout;

/** Keep the adaptive game image and all input surfaces in the same rectangle. */
final class GameViewport extends FrameLayout {
    GameViewport(Context context){super(context);}
    @Override protected void onMeasure(int widthSpec,int heightSpec) {
        int[] size=GameDisplayMetrics.fitGame(View.MeasureSpec.getSize(widthSpec),View.MeasureSpec.getSize(heightSpec));
        super.onMeasure(View.MeasureSpec.makeMeasureSpec(size[0],View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(size[1],View.MeasureSpec.EXACTLY));
    }
}
