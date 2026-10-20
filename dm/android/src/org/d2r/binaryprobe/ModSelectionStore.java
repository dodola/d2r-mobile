package org.d2r.binaryprobe;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Collections;

/** The current launch keeps its mounted resources/save; changes apply next launch. */
final class ModSelectionStore {
    private final File directory;
    ModSelectionStore(File files){directory=new File(files,"d2r-mobile/mods");}
    static boolean valid(String id){return id!=null&&id.matches("[a-z0-9_-]{1,48}");}
    private void ensure() throws IOException {
        if(Files.isSymbolicLink(directory.toPath())||(!directory.isDirectory()&&!directory.mkdirs()))throw new IOException("MOD directory unavailable");
    }
    ArrayList<String> installed() throws IOException {
        ensure();ArrayList<String> ids=new ArrayList<>();File[] children=directory.listFiles();
        if(children==null)throw new IOException("Cannot list MOD packages");
        for(File child:children)if(valid(child.getName())&&!Files.isSymbolicLink(child.toPath())&&child.isDirectory()&&new File(child,"package.txt").isFile())ids.add(child.getName());
        Collections.sort(ids);return ids;
    }
    String selected() throws IOException {
        ensure();File active=new File(directory,"active.txt");
        if(!active.exists())return "";
        if(Files.isSymbolicLink(active.toPath())||active.length()>64)throw new IOException("Invalid MOD selection");
        String text=new String(Files.readAllBytes(active.toPath()),StandardCharsets.US_ASCII);
        if(!text.endsWith("\n"))throw new IOException("Invalid MOD selection");
        String id=text.substring(0,text.length()-1);
        if(!id.isEmpty()&&!valid(id))throw new IOException("Invalid MOD selection");
        return id;
    }
    void select(String id) throws IOException {
        ensure();if(!id.isEmpty()&&(!valid(id)||!installed().contains(id)))throw new IOException("MOD package unavailable");
        File temp=new File(directory,"active.tmp"),active=new File(directory,"active.txt");
        if(Files.isSymbolicLink(temp.toPath())||Files.isSymbolicLink(active.toPath()))throw new IOException("Invalid MOD selection file");
        try(java.io.FileOutputStream out=new java.io.FileOutputStream(temp)){
            out.write((id+"\n").getBytes(StandardCharsets.US_ASCII));out.getFD().sync();
        }
        Files.move(temp.toPath(),active.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
    }
}
