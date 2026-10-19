package org.d2r.binaryprobe;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;

/** Immutable SAF transport envelope. The original SWPK1 payload remains unchanged. */
public final class SafCloudFormat {
    private SafCloudFormat(){}
    public static final int MAX_PACK_BYTES=64*1024*1024,MAX_PARENTS=128,MAX_GENERATIONS=4096,MAX_UNITS=256;
    public static final int MAX_GENERATION_BYTES=MAX_PACK_BYTES+MAX_PARENTS*32+512;
    private static final byte[] MAGIC={'D','2','R','S','G','1','\r','\n'},DONE={'C','O','M','P','L','E','T','E'};
    private static final byte[] PACK_MAGIC={'S','W','P','K','1','\n',0,0};
    public static final class Incomplete extends IOException {
        Incomplete(){super("Incomplete SAF generation");}
    }
    public static final class Pack {
        public final byte[] bytes;
        public final long stamp;
        public final boolean tomb;
        public final String hash;
        private Pack(byte[] bytes,long stamp,boolean tomb){this.bytes=bytes;this.stamp=stamp;this.tomb=tomb;hash=hex(sha256(bytes));}
    }
    public static final class Generation {
        public final String unit,id;
        public final List<String> parents;
        public final Pack pack;
        private Generation(String unit,String id,List<String> parents,Pack pack){this.unit=unit;this.id=id;this.parents=Collections.unmodifiableList(parents);this.pack=pack;}
    }
    public static final class Unit {
        public final Generation selected;
        public final List<Generation> heads;
        private Unit(List<Generation> heads){
            Collections.sort(heads,(a,b)->a.id.compareTo(b.id));this.heads=Collections.unmodifiableList(heads);
            Generation best=heads.get(0);for(Generation g:heads)if(compare(g,best)>0)best=g;selected=best;
        }
        public List<String> parentIds(){List<String> out=new ArrayList<>();for(Generation g:heads)out.add(g.id);return out;}
    }
    public static String providerId(String authority,String documentId)throws IOException{
        if(authority==null||authority.isEmpty()||documentId==null||documentId.isEmpty())throw new IOException("Invalid SAF provider identity");
        ByteArrayOutputStream b=new ByteArrayOutputStream();DataOutputStream d=new DataOutputStream(b);
        for(String s:new String[]{authority,documentId}){byte[] u=s.getBytes(StandardCharsets.UTF_8);d.writeInt(u.length);d.write(u);}
        return hex(sha256(b.toByteArray()));
    }
    public static void validUnit(String unit)throws IOException{
        if(unit==null||unit.isEmpty()||unit.getBytes(StandardCharsets.UTF_8).length>63||unit.indexOf('/')>=0||unit.indexOf('\\')>=0||unit.contains("..")||unit.charAt(0)=='.'||(unit.charAt(0)=='_'&&!unit.equals("_stash")))throw new IOException("Invalid cloud unit");
        for(int i=0;i<unit.length();i++)if(Character.isISOControl(unit.charAt(i))||unit.charAt(i)==':')throw new IOException("Invalid cloud unit");
    }
    private static String utf8(byte[] bytes)throws IOException{
        try{return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();}
        catch(CharacterCodingException e){throw new IOException("Invalid UTF-8 in cloud data",e);}
    }
    public static Pack parsePack(String unit,byte[] bytes)throws IOException{
        validUnit(unit);
        if(bytes==null||bytes.length<24||bytes.length>MAX_PACK_BYTES)throw new IOException("Invalid pack length");
        ByteBuffer b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);byte[] magic=new byte[8];b.get(magic);
        if(!Arrays.equals(magic,PACK_MAGIC))throw new IOException("Invalid SWPK magic");
        long stamp=b.getLong();int flags=b.getInt(),count=b.getInt();
        if((flags!=0&&flags!=1)||count<0||count>64||(flags==1&&count!=0)||(flags==0&&count==0))throw new IOException("Invalid SWPK flags/count");
        Set<String> paths=new HashSet<>();boolean character=false;
        for(int i=0;i<count;i++){
            if(b.remaining()<2)throw new IOException("Truncated SWPK path");int n=Short.toUnsignedInt(b.getShort());
            if(n==0||n>255||b.remaining()<n+12)throw new IOException("Invalid SWPK path length");
            byte[] p=new byte[n];b.get(p);String path=utf8(p);String[] parts=path.split("/",-1);
            if(parts.length>2||path.indexOf('\\')>=0||path.indexOf(':')>=0||!paths.add(path))throw new IOException("Invalid/duplicate SWPK path");
            for(String part:parts){if(part.isEmpty()||part.equals(".")||part.contains(".."))throw new IOException("Unsafe SWPK path");for(int k=0;k<part.length();k++)if(Character.isISOControl(part.charAt(k)))throw new IOException("Unsafe SWPK path");}
            if(unit.equals("_stash")){if(!parts[0].startsWith("SharedStash")&&!parts[0].startsWith("ModernSharedStash"))throw new IOException("Pack crosses stash boundary");}
            else{if(parts.length!=2||!parts[0].equals(unit))throw new IOException("Pack crosses character boundary");if(parts[1].equals(unit+".d2s"))character=true;}
            b.getLong();long size=Integer.toUnsignedLong(b.getInt());if(size>16*1024*1024||size>b.remaining())throw new IOException("Truncated/oversized SWPK file");b.position(b.position()+(int)size);
        }
        if(b.hasRemaining()||(flags==0&&!unit.equals("_stash")&&!character))throw new IOException("Incomplete or trailing SWPK data");
        return new Pack(bytes,stamp,flags==1);
    }
    public static byte[] encode(String unit,Collection<String> parents,byte[] packBytes)throws IOException{
        Pack pack=parsePack(unit,packBytes);TreeSet<String> p=new TreeSet<>(parents);
        if(p.size()!=parents.size()||p.size()>MAX_PARENTS)throw new IOException("Invalid generation parents");
        byte[] u=unit.getBytes(StandardCharsets.UTF_8);long length=28L+32+u.length+p.size()*32L+packBytes.length+40;
        ByteArrayOutputStream out=new ByteArrayOutputStream((int)length);DataOutputStream d=new DataOutputStream(out);
        d.write(MAGIC);d.writeLong(length);d.writeShort(u.length);d.writeShort(p.size());d.writeLong(packBytes.length);d.write(unhex(pack.hash));d.write(u);
        for(String id:p)d.write(unhex(id));d.write(packBytes);d.flush();byte[] body=out.toByteArray();d.write(sha256(body));d.write(DONE);d.flush();return out.toByteArray();
    }
    public static Generation decode(byte[] bytes)throws IOException{
        if(bytes==null||bytes.length<28)throw new Incomplete();
        if(bytes.length>MAX_GENERATION_BYTES)throw new IOException("Oversized generation");
        ByteBuffer b=ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN);byte[] magic=new byte[8];b.get(magic);
        if(!Arrays.equals(magic,MAGIC))throw new IOException("Unknown SAF generation format");
        long total=b.getLong();if(total<100||total>MAX_GENERATION_BYTES)throw new IOException("Invalid envelope length");if(total>bytes.length)throw new Incomplete();if(total!=bytes.length)throw new IOException("Trailing generation data");
        int unitLength=Short.toUnsignedInt(b.getShort()),count=Short.toUnsignedInt(b.getShort());long packLength=b.getLong();
        if(unitLength<1||unitLength>63||count>MAX_PARENTS||packLength<24||packLength>MAX_PACK_BYTES||total!=28L+32+unitLength+count*32L+packLength+40)throw new IOException("Invalid generation lengths");
        byte[] packHash=new byte[32],u=new byte[unitLength];b.get(packHash);b.get(u);String unit=utf8(u);validUnit(unit);
        List<String> parents=new ArrayList<>();String prior="";
        for(int i=0;i<count;i++){byte[] id=new byte[32];b.get(id);String next=hex(id);if(next.compareTo(prior)<=0)throw new IOException("Unsorted/duplicate generation parents");parents.add(next);prior=next;}
        byte[] packBytes=new byte[(int)packLength];b.get(packBytes);int bodyLength=b.position();byte[] bodyHash=new byte[32],done=new byte[8];b.get(bodyHash);b.get(done);
        if(!Arrays.equals(done,DONE)||!MessageDigest.isEqual(bodyHash,sha256(Arrays.copyOf(bytes,bodyLength)))||!MessageDigest.isEqual(packHash,sha256(packBytes)))throw new IOException("Generation hash/completion mismatch");
        String id=hex(bodyHash);if(parents.contains(id))throw new IOException("Self-parent generation");return new Generation(unit,id,parents,parsePack(unit,packBytes));
    }
    /** Remove causal ancestors first. Only genuinely concurrent heads use the original pack ordering. */
    public static Map<String,Unit> resolve(Collection<Generation> input)throws IOException{
        if(input.size()>MAX_GENERATIONS)throw new IOException("Too many generations");Map<String,Generation> all=new HashMap<>();
        for(Generation g:input){Generation old=all.put(g.id,g);if(old!=null&&(!old.unit.equals(g.unit)||!old.pack.hash.equals(g.pack.hash)))throw new IOException("Conflicting generation ID");}
        Map<String,Integer> color=new HashMap<>();for(Generation g:all.values())visit(g,all,color,0);
        Set<String> ancestors=new HashSet<>();for(Generation g:all.values())ancestors.addAll(g.parents);
        Map<String,List<Generation>> grouped=new TreeMap<>();for(Generation g:all.values())if(!ancestors.contains(g.id))grouped.computeIfAbsent(g.unit,k->new ArrayList<>()).add(g);
        if(grouped.size()>MAX_UNITS)throw new IOException("Too many cloud units");Map<String,Unit> out=new TreeMap<>();
        for(Map.Entry<String,List<Generation>> e:grouped.entrySet()){if(e.getValue().size()>MAX_PARENTS)throw new IOException("Too many concurrent heads");out.put(e.getKey(),new Unit(e.getValue()));}return out;
    }
    private static void visit(Generation g,Map<String,Generation> all,Map<String,Integer> color,int depth)throws IOException{
        Integer c=color.get(g.id);if(c!=null){if(c==1)throw new IOException("Cyclic generation graph");return;}
        if(depth>MAX_GENERATIONS)throw new IOException("Generation graph too deep");color.put(g.id,1);
        for(String id:g.parents){Generation p=all.get(id);if(p==null||!p.unit.equals(g.unit))throw new IOException("Missing/cross-unit generation parent");visit(p,all,color,depth+1);}color.put(g.id,2);
    }
    private static int compare(Generation a,Generation b){int c=Long.compareUnsigned(a.pack.stamp,b.pack.stamp);if(c==0)c=Boolean.compare(a.pack.tomb,b.pack.tomb);if(c==0)c=a.pack.hash.compareTo(b.pack.hash);return c==0?a.id.compareTo(b.id):c;}
    public static byte[] sha256(byte[] b){try{return MessageDigest.getInstance("SHA-256").digest(b);}catch(NoSuchAlgorithmException impossible){throw new AssertionError(impossible);}}
    public static String hex(byte[] b){StringBuilder s=new StringBuilder(b.length*2);for(byte x:b){s.append(Character.forDigit((x>>>4)&15,16));s.append(Character.forDigit(x&15,16));}return s.toString();}
    private static byte[] unhex(String s)throws IOException{if(s==null||!s.matches("[0-9a-f]{64}"))throw new IOException("Invalid SHA-256 ID");byte[] b=new byte[32];for(int i=0;i<32;i++)b[i]=(byte)((Character.digit(s.charAt(i*2),16)<<4)|Character.digit(s.charAt(i*2+1),16));return b;}
}
