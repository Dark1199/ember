// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdio>

#include "app/settings.h"

using namespace ember::app;

int main() {
    Settings d = ParseSettings("");
    assert(d.resolution_scale_percent == 100 && d.frame_cap == 0 && d.shadows);
    Settings s = ParseSettings("# c\nresolution_scale=60\nframe_cap = 30\r\nshadows=off\nbogus=1\n");
    assert(s.resolution_scale_percent == 60 && s.frame_cap == 30 && !s.shadows);
    s = ParseSettings("resolution_scale=5\nframe_cap=9999\nshadows=maybe");
    assert(s.resolution_scale_percent == 25 && s.frame_cap == 240 && s.shadows);
    s = ParseSettings("resolution_scale=abc\nframe_cap=\nresolution_scale=500");
    assert(s.resolution_scale_percent == 100 && s.frame_cap == 0);
    s = ParseSettings("shadows=1\nframe_cap=45"); // no trailing newline
    assert(s.frame_cap == 45 && s.shadows);
    const ScaledSize z = ScaleResolution(1920, 1080, 50);
    assert(z.width == 960 && z.height == 540);
    const ScaledSize full = ScaleResolution(1065, 495, 100);
    assert(full.width == 1065 && full.height == 495);
    const ScaledSize tiny = ScaleResolution(100, 40, 25);
    assert(tiny.width == 64 && tiny.height == 40); // never above the screen
    std::puts("settings_test: ok");
    return 0;
}
