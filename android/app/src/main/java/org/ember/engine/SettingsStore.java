// SPDX-License-Identifier: GPL-3.0-or-later
package org.ember.engine;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * The engine's user settings as a small text file the native code reads at startup
 * ({@code settings.txt}, one {@code key=value} per line). Values are clamped on both sides; the
 * native parser ignores anything it does not understand.
 */
final class SettingsStore {
    static final int[] RESOLUTION_STEPS = {25, 33, 50, 66, 75, 100};
    static final int[] FRAME_CAPS = {0, 30, 45, 60};

    int resolutionScale = 100;
    int frameCap = 0;
    boolean shadows = true;

    static SettingsStore parse(String text) {
        final SettingsStore s = new SettingsStore();
        for (String raw : text.split("\n")) {
            final String line = raw.trim();
            final int eq = line.indexOf('=');
            if (line.isEmpty() || line.startsWith("#") || eq < 0) continue;
            final String key = line.substring(0, eq).trim();
            final String value = line.substring(eq + 1).trim();
            try {
                if (key.equals("resolution_scale")) {
                    s.resolutionScale = Math.max(25, Math.min(100, Integer.parseInt(value)));
                } else if (key.equals("frame_cap")) {
                    s.frameCap = Math.max(0, Math.min(240, Integer.parseInt(value)));
                } else if (key.equals("shadows")) {
                    if (value.equals("0") || value.equals("off")) s.shadows = false;
                    else if (value.equals("1") || value.equals("on")) s.shadows = true;
                }
            } catch (NumberFormatException ignored) {
                // keep the default
            }
        }
        return s;
    }

    String serialize() {
        return "resolution_scale=" + resolutionScale + "\nframe_cap=" + frameCap + "\nshadows="
                + (shadows ? "on" : "off") + "\n";
    }

    static SettingsStore load(File dir) {
        try {
            return parse(new String(Files.readAllBytes(new File(dir, "settings.txt").toPath()),
                    StandardCharsets.UTF_8));
        } catch (IOException e) {
            return new SettingsStore();
        }
    }

    void save(File dir) throws IOException {
        final File tmp = new File(dir, "settings.txt.tmp");
        Files.write(tmp.toPath(), serialize().getBytes(StandardCharsets.UTF_8));
        Files.move(tmp.toPath(), new File(dir, "settings.txt").toPath(),
                java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }
}
