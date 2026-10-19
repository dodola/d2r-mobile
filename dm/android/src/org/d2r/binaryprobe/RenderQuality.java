package org.d2r.binaryprobe;

/** The game's internal 3D scale; the UI/output dimensions remain unchanged. */
final class RenderQuality {
    static final float AUTO=0.0f, ORIGINAL=1.0f, BALANCED=0.90f, PERFORMANCE=0.75f;
    static float parse(String text){
        String trimmed=text.trim();
        if(trimmed.equals("auto"))return AUTO;
        if(text.length()>63||!text.matches("[ \\t\\n\\r\\f\\x0B]*[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?[ \\t\\n\\r\\f\\x0B]*"))
            throw new IllegalArgumentException("画质设置格式无效");
        float scale=Float.parseFloat(text.trim());
        if(!Float.isFinite(scale)||scale<0.70f||scale>1.0f)throw new IllegalArgumentException("画质比例超出范围");
        return scale;
    }
    static String encode(float scale){
        return scale==AUTO?"auto\n":Float.toString(parse(Float.toString(scale)))+"\n";
    }
    static String description(float scale){
        if(scale==AUTO)return "自动（75% 起，掉帧时最低降至 60%）";
        if(scale==ORIGINAL)return "原画质（100%）";
        if(scale==BALANCED)return "均衡模式（90%）";
        if(scale==PERFORMANCE)return "性能模式（75%）";
        return "3D 比例 "+Math.round(scale*100)+"%";
    }
}
