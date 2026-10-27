// SPDX-License-Identifier: GPL-3.0-or-later
package org.ember.engine;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Streams a user-supplied game .zip and copies only the files the engine needs into the game
 * folder. No Android types are used here so the logic can be tested on a normal JVM.
 *
 * Decompression goes through java.util.zip, which is the platform's own zlib. The zip is read as
 * a stream (no random access), so it works directly on a document-picker stream and never holds
 * more than a 1 MiB buffer in memory.
 *
 * Kept: *.bhd5, *.bhd, *.bdt and DARKSOULS.exe, wherever they sit inside the zip. Files are
 * written flat into the game folder under their base name, which is also what makes path
 * traversal ("..\\..\\x") impossible: no directory component of an entry name is ever used.
 */
public final class ZipInstaller {
    public interface Listener {
        void line(String text);
        /** consumed = compressed bytes read so far; total = size of the zip, or -1 if unknown. */
        void progress(long consumed, long total);
    }

    public static final class Result {
        public int copied;
        public int ignored;          // entries that are not needed
        public int duplicates;       // wanted names seen a second time (first one is kept)
        public long bytesWritten;
        public boolean cancelled;
        public boolean sawIndex;     // at least one .bhd5/.bhd was copied
        public String error;         // null on success
        public boolean ok() { return error == null && !cancelled; }
    }

    private static final int BUFFER = 1 << 20;
    private static final long PROGRESS_INTERVAL_NANOS = 250_000_000L;

    private final File gameDir;
    private volatile boolean cancelled;

    public ZipInstaller(File gameDir) {
        this.gameDir = gameDir;
    }

    public void cancel() {
        cancelled = true;
    }

    /** The last path component, accepting both '/' and '\\'. */
    static String baseName(String entryName) {
        final int cut = Math.max(entryName.lastIndexOf('/'), entryName.lastIndexOf('\\'));
        return cut < 0 ? entryName : entryName.substring(cut + 1);
    }

    static boolean isWanted(String baseName) {
        if (baseName.isEmpty() || baseName.indexOf('\0') >= 0) {
            return false;
        }
        final String n = baseName.toLowerCase(Locale.ROOT);
        return n.endsWith(".bhd5") || n.endsWith(".bhd") || n.endsWith(".bdt")
                || n.equals("darksouls.exe");
    }

    public Result install(InputStream zip, long totalBytes, Listener listener) {
        final Result result = new Result();
        if (!gameDir.isDirectory() && !gameDir.mkdirs()) {
            result.error = "cannot create " + gameDir;
            return result;
        }
        final Counting counting = new Counting(zip);
        final byte[] buffer = new byte[BUFFER];
        final Set<String> seen = new HashSet<>();
        long lastReport = 0;
        File part = null;
        try (ZipInputStream in = new ZipInputStream(new BufferedInputStream(counting, 1 << 16))) {
            ZipEntry entry;
            while ((entry = in.getNextEntry()) != null) {
                if (cancelled) {
                    result.cancelled = true;
                    break;
                }
                if (entry.isDirectory()) {
                    continue;
                }
                final String base = baseName(entry.getName());
                if (!isWanted(base)) {
                    result.ignored++;
                    continue;
                }
                if (!seen.add(base.toLowerCase(Locale.ROOT))) {
                    result.duplicates++;
                    listener.line("duplicate ignored (first copy kept): " + entry.getName());
                    continue;
                }
                listener.line("extracting " + base);
                final File target = new File(gameDir, base);
                part = new File(gameDir, base + ".part");
                long written = 0;
                try (OutputStream out = new FileOutputStream(part)) {
                    int n;
                    while ((n = in.read(buffer)) > 0) {
                        if (cancelled) {
                            break;
                        }
                        out.write(buffer, 0, n);
                        written += n;
                        final long now = System.nanoTime();
                        if (now - lastReport > PROGRESS_INTERVAL_NANOS) {
                            lastReport = now;
                            listener.progress(counting.count, totalBytes);
                        }
                    }
                }
                if (cancelled) {
                    result.cancelled = true;
                    break;
                }
                if (target.exists() && !target.delete()) {
                    throw new IOException("cannot replace " + target);
                }
                if (!part.renameTo(target)) {
                    throw new IOException("cannot move " + part.getName() + " into place");
                }
                part = null;
                result.copied++;
                result.bytesWritten += written;
                if (base.toLowerCase(Locale.ROOT).endsWith(".bhd5")
                        || base.toLowerCase(Locale.ROOT).endsWith(".bhd")) {
                    result.sawIndex = true;
                }
                listener.line("  " + base + ": " + written + " bytes");
            }
        } catch (IOException e) {
            // Includes "No space left on device" and a truncated or corrupt zip.
            result.error = e.getClass().getSimpleName() + ": " + e.getMessage();
        } finally {
            if (part != null) {
                //noinspection ResultOfMethodCallIgnored
                part.delete(); // never leave a half-written file behind
            }
        }
        listener.progress(counting.count, totalBytes);
        return result;
    }

    private static final class Counting extends FilterInputStream {
        long count;

        Counting(InputStream in) {
            super(in);
        }

        @Override
        public int read() throws IOException {
            final int b = super.read();
            if (b >= 0) {
                count++;
            }
            return b;
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            final int n = super.read(b, off, len);
            if (n > 0) {
                count += n;
            }
            return n;
        }
    }
}
