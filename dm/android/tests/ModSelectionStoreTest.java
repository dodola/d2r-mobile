package org.d2r.binaryprobe;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
public final class ModSelectionStoreTest {
    private static int checks;
    private static void check(boolean value){checks++;if(!value)throw new AssertionError("check "+checks);}
    public static void main(String[] args) throws Exception {
        File files=Files.createTempDirectory("d2r-mod-selection-").toFile();
        ModSelectionStore store=new ModSelectionStore(files);
        check(store.selected().isEmpty());check(store.installed().isEmpty());
        File mods=new File(files,"d2r-mobile/mods"),sample=new File(mods,"sample");check(sample.mkdir());
        Files.write(new File(sample,"package.txt").toPath(),"prepared fixture\n".getBytes(StandardCharsets.US_ASCII));
        File incomplete=new File(mods,"incomplete");check(incomplete.mkdir());
        check(store.installed().size()==1&&store.installed().get(0).equals("sample"));
        store.select("sample");check(store.selected().equals("sample"));
        check(new String(Files.readAllBytes(new File(mods,"active.txt").toPath()),StandardCharsets.US_ASCII).equals("sample\n"));
        for(String id:new String[]{"../sample","MIXED","missing","a;b","sample\n"}){
            try{store.select(id);throw new AssertionError("accepted "+id);}catch(IOException expected){checks++;}
            check(store.selected().equals("sample"));
        }
        store.select("");check(store.selected().isEmpty());
        Files.write(new File(mods,"active.txt").toPath(),"sample\nextra\n".getBytes(StandardCharsets.US_ASCII));
        try{store.selected();throw new AssertionError("accepted multiple lines");}catch(IOException expected){checks++;}
        store.select("");check(store.selected().isEmpty());
        System.out.println("MOD_SELECTION checks="+checks+" passed=1 device_contacted=0");
    }
}
