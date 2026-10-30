// SPDX-License-Identifier: GPL-3.0-or-later
package org.ember.engine;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class EmberJavaTests {
    static void check(boolean ok, String what) {
        if (!ok) {
            throw new AssertionError(what);
        }
    }

    static byte[] data(int size, int seed) {
        byte[] b = new byte[size];
        for (int i = 0; i < size; i++) b[i] = (byte) (i * 31 + seed);
        return b;
    }

    static byte[] noise(int size, long seed) {
        byte[] b = new byte[size];
        new java.util.Random(seed).nextBytes(b); // incompressible, so the zip really is large
        return b;
    }

    static byte[] zip(Object... nameAndData) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(bytes)) {
            for (int i = 0; i < nameAndData.length; i += 2) {
                String name = (String) nameAndData[i];
                z.putNextEntry(new ZipEntry(name));
                if (!name.endsWith("/")) z.write((byte[]) nameAndData[i + 1]);
                z.closeEntry();
            }
        }
        return bytes.toByteArray();
    }

    static final ZipInstaller.Listener QUIET = new ZipInstaller.Listener() {
        public void line(String text) {}
        public void progress(long consumed, long total) {}
    };

    static void installerTests() throws IOException {
        File root = Files.createTempDirectory("ember_java_test").toFile();
        File game = new File(root, "game");

        byte[] index = data(5000, 1), bdt = noise(300_000, 2), exe = data(1234, 3);
        byte[] zipBytes = zip(
                "Dark Souls/", null,
                "Dark Souls/DATA/dvdbnd0.bhd5", index,
                "Dark Souls\\DATA\\DVDBND0.BDT", bdt,
                "Dark Souls/DARKSOULS.exe", exe,
                "Dark Souls/readme.txt", data(10, 4),
                "Dark Souls/map/m10.msb", data(10, 5),
                "Dark Souls/other/dvdbnd0.bhd5", data(77, 6),  // duplicate base name
                "../../escape.bdt", data(100, 7),               // traversal attempt
                "C:\\evil\\payload.bhd", data(200, 8));
        ZipInstaller installer = new ZipInstaller(game);
        ZipInstaller.Result r = installer.install(new ByteArrayInputStream(zipBytes), zipBytes.length, QUIET);
        check(r.ok() && r.error == null, "install ok: " + r.error);
        check(r.copied == 5, "copied " + r.copied);           // bhd5, bdt, exe, escape.bdt, payload.bhd
        check(r.duplicates == 1 && r.ignored == 2, "dup/ignored " + r.duplicates + "/" + r.ignored);
        check(r.sawIndex, "saw index");
        check(Arrays.equals(Files.readAllBytes(new File(game, "dvdbnd0.bhd5").toPath()), index), "index bytes");
        check(Arrays.equals(Files.readAllBytes(new File(game, "DVDBND0.BDT").toPath()), bdt), "bdt bytes");
        check(Arrays.equals(Files.readAllBytes(new File(game, "DARKSOULS.exe").toPath()), exe), "exe bytes");
        check(new File(game, "escape.bdt").isFile(), "traversal flattened into the game folder");
        check(!new File(root, "escape.bdt").exists(), "nothing escaped the game folder");
        check(!new File(game, "readme.txt").exists() && !new File(game, "m10.msb").exists(), "unwanted skipped");
        for (String name : game.list()) check(!name.endsWith(".part"), "no .part left: " + name);
        check(r.bytesWritten == 5000 + 300_000 + 1234 + 100 + 200, "bytes " + r.bytesWritten);

        // Re-running replaces files rather than failing.
        r = new ZipInstaller(game).install(new ByteArrayInputStream(zipBytes), zipBytes.length, QUIET);
        check(r.ok() && r.copied == 5, "reinstall");

        // Not a zip at all: nothing copied, no error (a zip with no entries), sawIndex false.
        r = new ZipInstaller(new File(root, "g2")).install(new ByteArrayInputStream("hello".getBytes(StandardCharsets.UTF_8)), 5, QUIET);
        check(r.copied == 0 && !r.sawIndex, "garbage input copies nothing");

        // Truncated zip: error reported, partial file removed.
        File g3 = new File(root, "g3");
        check(zipBytes.length > 200_000, "zip is large enough to cut");
        byte[] cut = Arrays.copyOf(zipBytes, 100_000);
        r = new ZipInstaller(g3).install(new ByteArrayInputStream(cut), cut.length, QUIET);
        check(r.error != null, "truncated zip reports an error");
        for (String name : g3.list()) check(!name.endsWith(".part"), "partial removed: " + name);

        // Cancel before start: nothing extracted.
        ZipInstaller cancelled = new ZipInstaller(new File(root, "g4"));
        cancelled.cancel();
        r = cancelled.install(new ByteArrayInputStream(zipBytes), zipBytes.length, QUIET);
        check(r.cancelled && r.copied == 0 && !r.ok(), "cancel");

        check(ZipInstaller.baseName("a\\b/c.bdt").equals("c.bdt"), "baseName");
        check(ZipInstaller.isWanted("X.BHD5") && ZipInstaller.isWanted("darksouls.EXE")
                && !ZipInstaller.isWanted("other.exe") && !ZipInstaller.isWanted(""), "isWanted");
    }

    static void append(File f, String s) throws IOException {
        try (FileOutputStream o = new FileOutputStream(f, true)) { o.write(s.getBytes(StandardCharsets.UTF_8)); }
    }

    static void tailTests() throws IOException {
        File dir = Files.createTempDirectory("ember_tail_test").toFile();
        File log = new File(dir, "ember.log");
        LogTail tail = new LogTail(log, 1000);
        check(tail.poll() == null, "missing file");
        append(log, "one\ntwo\n");
        check("one\ntwo\n".equals(tail.poll()), "first poll");
        check(tail.poll() == null, "nothing new");
        append(log, "thr");
        check(tail.poll() == null, "partial line withheld");
        append(log, "ee\nfour\n");
        check("three\nfour\n".equals(tail.poll()), "line completed");

        // New launch: the file is replaced by a shorter one.
        check(log.delete(), "delete");
        append(log, "A\n");
        String restarted = tail.poll();
        check(restarted != null && restarted.startsWith("--- new log started ---") && restarted.endsWith("A\n"), "restart: " + restarted);

        // Large backlog: only the tail is shown, starting on a line boundary.
        File big = new File(dir, "big.log");
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 2000; i++) sb.append("line ").append(i).append('\n');
        append(big, sb.toString());
        String shown = new LogTail(big, 500).poll();
        check(shown.startsWith("... (older output not shown)\nline "), "backlog prefix");
        check(shown.endsWith("line 1999\n"), "backlog end");
        check(shown.length() < 700, "backlog bounded " + shown.length());
    }

    static void settingsTests() throws IOException {
        SettingsStore d = SettingsStore.parse("");
        check(d.resolutionScale == 100 && d.frameCap == 0 && d.shadows, "settings defaults");
        SettingsStore s = SettingsStore.parse("resolution_scale=50\nframe_cap=30\nshadows=off\nx=1\n");
        check(s.resolutionScale == 50 && s.frameCap == 30 && !s.shadows, "settings parse");
        s = SettingsStore.parse("resolution_scale=1\nframe_cap=abc\nshadows=maybe");
        check(s.resolutionScale == 25 && s.frameCap == 0 && s.shadows, "settings clamp and ignore");
        File dir = Files.createTempDirectory("ember-settings").toFile();
        s.resolutionScale = 66; s.frameCap = 45; s.shadows = false;
        s.save(dir);
        SettingsStore back = SettingsStore.load(dir);
        check(back.resolutionScale == 66 && back.frameCap == 45 && !back.shadows, "settings round trip");
        check(SettingsStore.load(new File(dir, "missing")).resolutionScale == 100, "settings missing dir");
    }

    public static void main(String[] args) throws Exception {
        installerTests();
        tailTests();
        settingsTests();
        System.out.println("EmberJavaTests: ok");
    }
}
