// SPDX-License-Identifier: GPL-3.0-or-later
package org.ember.engine;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;

/**
 * Follows a growing log file (like `tail -f`). Call poll() periodically; it returns the text
 * appended since the last call, or null. Only complete lines are returned. If the file shrinks
 * (the engine starts a fresh log on every launch) the tail restarts from the beginning.
 */
public final class LogTail {
    private static final int MAX_CHUNK = 64 * 1024;

    private final File file;
    private final int initialBytes;
    private long offset = -1;

    public LogTail(File file, int initialBytes) {
        this.file = file;
        this.initialBytes = initialBytes;
    }

    public String poll() {
        final long length = file.length(); // 0 when the file does not exist
        String prefix = "";
        if (offset < 0) {
            if (length == 0) {
                return null;
            }
            offset = Math.max(0, length - initialBytes);
            if (offset > 0) {
                prefix = "... (older output not shown)\n";
                offset = skipToLineStart(offset);
            }
        } else if (length < offset) {
            offset = 0;
            prefix = "--- new log started ---\n";
        }
        if (length == offset) {
            return prefix.isEmpty() ? null : prefix;
        }
        final int want = (int) Math.min(length - offset, MAX_CHUNK);
        final byte[] data = new byte[want];
        try (RandomAccessFile raf = new RandomAccessFile(file, "r")) {
            raf.seek(offset);
            raf.readFully(data);
        } catch (IOException e) {
            return prefix.isEmpty() ? null : prefix;
        }
        int usable = want;
        if (want == MAX_CHUNK || offset + want < length) {
            // More data follows; take it all (a very long line is split rather than stalling).
        } else {
            usable = 0;
            for (int i = want - 1; i >= 0; --i) {
                if (data[i] == '\n') {
                    usable = i + 1;
                    break;
                }
            }
        }
        if (usable == 0) {
            return prefix.isEmpty() ? null : prefix; // a partial line: wait for its newline
        }
        offset += usable;
        return prefix + new String(data, 0, usable, StandardCharsets.UTF_8);
    }

    private long skipToLineStart(long from) {
        try (RandomAccessFile raf = new RandomAccessFile(file, "r")) {
            raf.seek(from);
            for (int i = 0; i < 4096; ++i) {
                final int b = raf.read();
                if (b < 0) {
                    break;
                }
                if (b == '\n') {
                    return from + i + 1;
                }
            }
        } catch (IOException ignored) {
            // fall through
        }
        return from;
    }
}
