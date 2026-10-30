// SPDX-License-Identifier: GPL-3.0-or-later
package org.ember.engine;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.InputStream;
import java.util.Locale;

/**
 * Launcher screen: installs the user's game files from a .zip, shows engine status, tails the
 * engine's ember.log, and starts the native engine. Built from plain views (no libraries) so the
 * APK stays small and the build has no extra dependencies.
 */
public final class MainActivity extends Activity {
    private static final int REQUEST_PICK_ZIP = 1;
    private static final int LOG_POLL_MS = 500;
    private static final int CONSOLE_MAX_CHARS = 120_000;
    private static final int CONSOLE_KEEP_CHARS = 80_000;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private File filesDir;
    private File gameDir;
    private LogTail logTail;
    private volatile ZipInstaller installer;

    private TextView status;
    private TextView console;
    private ScrollView consoleScroll;
    private ProgressBar progress;
    private Button pickButton;
    private Button cancelButton;
    private Button launchButton;
    private Button verifyButton;

    private final Runnable logPoller = new Runnable() {
        @Override
        public void run() {
            final String text = logTail.poll();
            if (text != null) {
                appendConsole(text);
            }
            ui.postDelayed(this, LOG_POLL_MS);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // The same directory the native engine reports as externalDataPath.
        final File external = getExternalFilesDir(null);
        filesDir = external != null ? external : getFilesDir();
        gameDir = new File(filesDir, "game");
        //noinspection ResultOfMethodCallIgnored
        gameDir.mkdirs();
        logTail = new LogTail(new File(filesDir, "ember.log"), 16 * 1024);
        buildUi();
        appendConsole("Ember launcher. Game folder: " + gameDir + "\n");
        refreshStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshStatus();
        ui.post(logPoller);
    }

    @Override
    protected void onPause() {
        ui.removeCallbacks(logPoller);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        final ZipInstaller running = installer;
        if (running != null && isFinishing()) {
            running.cancel();
        }
        super.onDestroy();
    }

    // ------------------------------------------------------------------------------- actions

    private void pickZip() {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                "application/zip", "application/x-zip-compressed", "application/octet-stream"});
        startActivityForResult(intent, REQUEST_PICK_ZIP);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_PICK_ZIP && resultCode == RESULT_OK && data != null
                && data.getData() != null) {
            startInstall(data.getData());
        }
    }

    private void startInstall(final Uri uri) {
        if (installer != null) {
            return;
        }
        final long total = querySize(uri);
        final InputStream stream;
        try {
            stream = getContentResolver().openInputStream(uri);
        } catch (Exception e) {
            appendConsole("[install] cannot open the file: " + e + "\n");
            return;
        }
        if (stream == null) {
            appendConsole("[install] cannot open the file\n");
            return;
        }
        final ZipInstaller worker = new ZipInstaller(gameDir);
        installer = worker;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setBusy(true);
        appendConsole("[install] reading " + (total >= 0 ? formatBytes(total) : "unknown size")
                + " zip; only index/data archives are copied\n");

        final Thread thread = new Thread(() -> {
            final ZipInstaller.Result result = worker.install(stream, total,
                    new ZipInstaller.Listener() {
                        @Override
                        public void line(String text) {
                            ui.post(() -> appendConsole("[install] " + text + "\n"));
                        }

                        @Override
                        public void progress(long consumed, long totalBytes) {
                            ui.post(() -> showProgress(consumed, totalBytes));
                        }
                    });
            try {
                stream.close();
            } catch (Exception ignored) {
                // nothing useful to do
            }
            ui.post(() -> finishInstall(result));
        }, "ember-zip-install");
        thread.start();
    }

    private void finishInstall(ZipInstaller.Result result) {
        installer = null;
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setBusy(false);
        if (result.cancelled) {
            appendConsole("[install] cancelled\n");
        } else if (result.error != null) {
            appendConsole("[install] FAILED: " + result.error + "\n");
        } else {
            appendConsole(String.format(Locale.ROOT,
                    "[install] done: %d file(s), %s written, %d unneeded entries skipped\n",
                    result.copied, formatBytes(result.bytesWritten), result.ignored));
            if (!result.sawIndex) {
                appendConsole("[install] WARNING: no .bhd5/.bhd index file was found in this zip\n");
            }
        }
        refreshStatus();
    }

    /**
     * Asks the engine to run its full integrity pass on the next start (it reads every archive
     * entry and fully decompresses every compressed stream), then starts it. The result is
     * written to ember.log and therefore shows up in the console below.
     */
    private void verifyThenLaunch() {
        try {
            if (!new File(filesDir, "verify.request").createNewFile()) {
                appendConsole("[verify] request file already present\n");
            }
        } catch (java.io.IOException e) {
            appendConsole("[verify] cannot request verification: " + e + "\n");
            return;
        }
        appendConsole("[verify] starting the engine with a full file check; this can take a minute\n");
        launchEngine();
    }

    private void launchEngine() {
        startActivity(new Intent(this, android.app.NativeActivity.class));
    }

    // ------------------------------------------------------------------------------- helpers

    private long querySize(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.SIZE},
                null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) {
                return c.getLong(0);
            }
        } catch (Exception ignored) {
            // size is only used for the progress bar
        }
        return -1;
    }

    private void refreshStatus() {
        int indexes = 0;
        int data = 0;
        final File[] files = gameDir.listFiles();
        if (files != null) {
            for (File f : files) {
                final String n = f.getName().toLowerCase(Locale.ROOT);
                if (n.endsWith(".bhd5") || n.endsWith(".bhd")) {
                    indexes++;
                } else if (n.endsWith(".bdt")) {
                    data++;
                }
            }
        }
        status.setText(String.format(Locale.ROOT, "Game files: %d index, %d data  •  %s%s",
                indexes, data, indexes > 0 && data > 0 ? "ready" : "not installed",
                lastVerifyResult()));
    }

    /** Last line of the kept verification report, e.g. "verify: every archive entry ... cleanly". */
    private String lastVerifyResult() {
        final File report = new File(filesDir, "verify_result.txt");
        if (!report.isFile() || report.length() > 256 * 1024) {
            return "";
        }
        String last = "";
        try (java.io.BufferedReader in = new java.io.BufferedReader(new java.io.FileReader(report))) {
            String line;
            while ((line = in.readLine()) != null) {
                if (line.startsWith("verify: ")) {
                    last = line;
                }
            }
        } catch (java.io.IOException ignored) {
            return "";
        }
        return last.isEmpty() ? "" : "\n" + (last.length() > 90 ? last.substring(0, 90) + "..." : last);
    }

    private void setBusy(boolean busy) {
        pickButton.setEnabled(!busy);
        launchButton.setEnabled(!busy);
        verifyButton.setEnabled(!busy);
        cancelButton.setVisibility(busy ? View.VISIBLE : View.GONE);
        progress.setVisibility(busy ? View.VISIBLE : View.GONE);
        if (busy) {
            progress.setIndeterminate(true);
        }
    }

    private void showProgress(long consumed, long total) {
        if (installer == null) {
            return;
        }
        if (total > 0) {
            progress.setIndeterminate(false);
            progress.setMax(1000);
            progress.setProgress((int) Math.min(1000, consumed * 1000 / total));
        }
    }

    private void appendConsole(String text) {
        final boolean atBottom = consoleScroll == null
                || consoleScroll.getChildAt(0).getBottom()
                        - (consoleScroll.getHeight() + consoleScroll.getScrollY()) < dp(48);
        console.append(text);
        if (console.length() > CONSOLE_MAX_CHARS) {
            final CharSequence all = console.getText();
            console.setText(all.subSequence(all.length() - CONSOLE_KEEP_CHARS, all.length()));
        }
        if (atBottom) {
            consoleScroll.post(() -> consoleScroll.fullScroll(View.FOCUS_DOWN));
        }
    }

    private static String formatBytes(long bytes) {
        if (bytes < 1024) {
            return bytes + " B";
        }
        final double mib = bytes / (1024.0 * 1024.0);
        return mib < 1024 ? String.format(Locale.ROOT, "%.1f MiB", mib)
                : String.format(Locale.ROOT, "%.2f GiB", mib / 1024.0);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    // ---------------------------------------------------------------------------------- UI

    private static final int BACKGROUND = Color.rgb(0x14, 0x10, 0x0E);
    private static final int SURFACE = Color.rgb(0x1E, 0x18, 0x15);
    private static final int ACCENT = Color.rgb(0xE8, 0x65, 0x0A);
    private static final int TEXT = Color.rgb(0xEE, 0xE6, 0xDF);
    private static final int TEXT_DIM = Color.rgb(0xA0, 0x94, 0x8A);

    private Button makeButton(String label, boolean primary) {
        final Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(primary ? Color.BLACK : TEXT);
        b.setBackgroundColor(primary ? ACCENT : SURFACE);
        return b;
    }

    private void buildUi() {
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BACKGROUND);
        root.setPadding(dp(16), dp(16), dp(16), dp(16));
        // Keep clear of the status bar and navigation bar on every Android version.
        root.setFitsSystemWindows(true);

        final LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        final ImageView logo = new ImageView(this);
        logo.setImageResource(R.drawable.ic_ember_foreground);
        logo.setBackgroundColor(SURFACE);
        header.addView(logo, new LinearLayout.LayoutParams(dp(64), dp(64)));
        final LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        titles.setPadding(dp(14), 0, 0, 0);
        final TextView title = new TextView(this);
        title.setText("Ember");
        title.setTextColor(TEXT);
        title.setTextSize(26);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        status = new TextView(this);
        status.setTextColor(TEXT_DIM);
        status.setTextSize(13);
        titles.addView(title);
        titles.addView(status);
        header.addView(titles);
        root.addView(header);

        final LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setPadding(0, dp(16), 0, dp(8));
        pickButton = makeButton("Install game .zip", false);
        launchButton = makeButton("Launch", true);
        cancelButton = makeButton("Cancel", false);
        cancelButton.setVisibility(View.GONE);
        final LinearLayout.LayoutParams weight =
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        weight.setMargins(0, 0, dp(8), 0);
        buttons.addView(pickButton, weight);
        buttons.addView(cancelButton, weight);
        buttons.addView(launchButton, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(buttons);

        verifyButton = makeButton("Verify installed files (slow: full check of all game data)", false);
        root.addView(verifyButton, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        final Button settingsButton = makeButton("Settings (resolution, frame cap, shadows)", false);
        root.addView(settingsButton, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        settingsButton.setOnClickListener(v ->
                startActivity(new android.content.Intent(this, SettingsActivity.class)));

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setVisibility(View.GONE);
        root.addView(progress, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        console = new TextView(this);
        console.setTypeface(Typeface.MONOSPACE);
        console.setTextSize(11);
        console.setTextColor(TEXT);
        console.setTextIsSelectable(true);
        console.setPadding(dp(10), dp(10), dp(10), dp(10));
        consoleScroll = new ScrollView(this);
        consoleScroll.setBackgroundColor(SURFACE);
        consoleScroll.addView(console);
        final LinearLayout.LayoutParams fill =
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        fill.setMargins(0, dp(8), 0, 0);
        root.addView(consoleScroll, fill);

        pickButton.setOnClickListener(v -> pickZip());
        launchButton.setOnClickListener(v -> launchEngine());
        verifyButton.setOnClickListener(v -> verifyThenLaunch());
        cancelButton.setOnClickListener(v -> {
            final ZipInstaller running = installer;
            if (running != null) {
                running.cancel();
            }
        });
        setContentView(root);
    }
}
