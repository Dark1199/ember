// SPDX-License-Identifier: GPL-3.0-or-later
package org.ember.engine;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;

/** Resolution scale, frame cap and shadow toggle; saved to settings.txt, read at engine start. */
public final class SettingsActivity extends Activity {
    private static final int BACKGROUND = Color.rgb(0x14, 0x10, 0x0E);
    private static final int TEXT = Color.rgb(0xEE, 0xE6, 0xDF);
    private static final int DIM = Color.rgb(0xA0, 0x94, 0x8A);

    private File dir;
    private SettingsStore settings;
    private RadioGroup scaleGroup;
    private RadioGroup capGroup;
    private CheckBox shadows;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final File external = getExternalFilesDir(null);
        dir = external != null ? external : getFilesDir();
        settings = SettingsStore.load(dir);

        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BACKGROUND);
        final int pad = (int) (16 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);
        root.setFitsSystemWindows(true);

        root.addView(label("Resolution scale (share of the screen's pixels per side; lower is cooler and faster)", true));
        scaleGroup = new RadioGroup(this);
        for (int step : SettingsStore.RESOLUTION_STEPS) {
            scaleGroup.addView(radio(step + "%", step, settings.resolutionScale == step));
        }
        root.addView(scaleGroup);

        root.addView(label("Frame cap", true));
        capGroup = new RadioGroup(this);
        for (int cap : SettingsStore.FRAME_CAPS) {
            capGroup.addView(radio(cap == 0 ? "Display refresh rate" : cap + " fps", cap, settings.frameCap == cap));
        }
        root.addView(capGroup);

        shadows = new CheckBox(this);
        shadows.setText("Shadows (the engine has no shadow pass yet; the choice is saved for when it does)");
        shadows.setTextColor(TEXT);
        shadows.setChecked(settings.shadows);
        root.addView(shadows);

        final Button save = new Button(this);
        save.setText("Save");
        save.setAllCaps(false);
        save.setOnClickListener(v -> save());
        root.addView(save);
        root.addView(label("Takes effect the next time the engine starts.", false));

        final ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BACKGROUND);
        scroll.addView(root);
        setContentView(scroll);
    }

    private TextView label(String text, boolean heading) {
        final TextView t = new TextView(this);
        t.setText(text);
        t.setTextColor(heading ? TEXT : DIM);
        t.setTextSize(heading ? 15 : 12);
        t.setPadding(0, heading ? 24 : 12, 0, 8);
        return t;
    }

    private RadioButton radio(String text, int value, boolean checked) {
        final RadioButton b = new RadioButton(this);
        b.setText(text);
        b.setTextColor(TEXT);
        b.setTag(value);
        b.setId(generateId(value));
        b.setChecked(checked);
        return b;
    }

    private static int generateId(int value) {
        return 1000 + value;
    }

    private void save() {
        final RadioButton scale = findViewById(scaleGroup.getCheckedRadioButtonId());
        final RadioButton cap = findViewById(capGroup.getCheckedRadioButtonId());
        if (scale != null) settings.resolutionScale = (Integer) scale.getTag();
        if (cap != null) settings.frameCap = (Integer) cap.getTag();
        settings.shadows = shadows.isChecked();
        try {
            settings.save(dir);
            Toast.makeText(this, "Saved", Toast.LENGTH_SHORT).show();
            finish();
        } catch (IOException e) {
            Toast.makeText(this, "Could not save: " + e, Toast.LENGTH_LONG).show();
        }
    }
}
