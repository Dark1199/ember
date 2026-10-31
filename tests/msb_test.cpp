// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the MSB reader. The fixture is built here from the layout in docs/formats/msb.md,
// so these tests prove the parser follows that description and rejects damaged input; they cannot
// prove the description matches real files. The startup log line (`msb: ...`) does that.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "formats/msb.h"

using namespace ember::formats;
using Bytes = std::vector<std::byte>;

namespace {

struct Out {
    Bytes b;
    template <typename T>
    void Put(T v) {
        std::uint8_t raw[sizeof(T)];
        std::memcpy(raw, &v, sizeof(T));
        for (auto c : raw) b.push_back(static_cast<std::byte>(c));
    }
    void PutAt32(size_t at, std::int32_t v) { std::memcpy(b.data() + at, &v, 4); }
    void Str(const std::string& s) {
        for (char c : s) b.push_back(static_cast<std::byte>(c));
        b.push_back(std::byte{0});
    }
    size_t Pos() const { return b.size(); }
};

struct Spec {
    int model_count = 2;
    int part_count = 3;
    std::uint32_t bad_model_ref = 0xFFFFFFFF; // part 2 uses this index when != 0xFFFFFFFF
    bool nan_position = false;
};

// Chain: MODEL, EVENT (empty), POINT (empty), PARTS. Strings live after each table's data.
Bytes Build(const Spec& s) {
    Out o;
    const auto table = [&](const char* name, int entries, auto write_entries) {
        const size_t start = o.Pos();
        o.Put<std::int32_t>(0);
        o.Put<std::int32_t>(entries + 1);
        o.Put<std::int32_t>(0); // name offset, patched
        const size_t list = o.Pos();
        for (int i = 0; i <= entries; ++i) o.Put<std::int32_t>(0);
        o.PutAt32(start + 8, static_cast<std::int32_t>(o.Pos()));
        o.Str(name);
        while (o.Pos() % 4) o.b.push_back(std::byte{0});
        for (int i = 0; i < entries; ++i) {
            o.PutAt32(list + 4 * i, static_cast<std::int32_t>(o.Pos()));
            write_entries(i);
        }
        return std::pair<size_t, size_t>{list, entries};
    };
    std::vector<std::pair<size_t, size_t>> lists;
    lists.push_back(table("MODEL_PARAM_ST", s.model_count, [&](int i) {
        const size_t at = o.Pos();
        o.Put<std::int32_t>(0x20); // name directly after the 0x20 header
        o.Put<std::int32_t>(0);    // map piece
        o.Put<std::int32_t>(i);
        const std::string model_name = i == 0 ? "m0000B0A10" : "m0001B0A10";
        o.Put<std::int32_t>(0x20 + static_cast<std::int32_t>(model_name.size()) + 1);
        o.Put<std::int32_t>(1);
        for (int k = 0; k < 3; ++k) o.Put<std::int32_t>(0);
        assert(o.Pos() - at == 0x20);
        o.Str(model_name);
        o.Str("sib");
        while (o.Pos() % 4) o.b.push_back(std::byte{0});
    }));
    lists.push_back(table("EVENT_PARAM_ST", 0, [](int) {}));
    lists.push_back(table("POINT_PARAM_ST", 0, [](int) {}));
    lists.push_back(table("PARTS_PARAM_ST", s.part_count, [&](int i) {
        const size_t at = o.Pos();
        o.Put<std::int32_t>(0x64); // name after the header
        o.Put<std::int32_t>(i == 2 ? 2 : 0);
        o.Put<std::int32_t>(i);
        o.Put<std::uint32_t>(i == 2 && s.bad_model_ref != 0xFFFFFFFF ? s.bad_model_ref : (i % s.model_count));
        const std::string part_name = "part_" + std::to_string(i);
        o.Put<std::int32_t>(0x64 + static_cast<std::int32_t>(part_name.size()) + 1);
        const float pos[3] = {10.0f * i, 2.0f, -5.0f};
        for (int k = 0; k < 3; ++k) o.Put<float>(k == 0 && s.nan_position && i == 1 ? std::nanf("") : pos[k]);
        for (float f : {0.0f, 90.0f, 0.0f}) o.Put<float>(f);
        for (float f : {1.0f, 1.0f, 1.0f}) o.Put<float>(f);
        for (int k = 0; k < 8; ++k) o.Put<std::uint32_t>(k == 0 ? 0xFFFFFFFFu : 0u);
        o.Put<std::int32_t>(0);
        o.Put<std::int32_t>(0);
        o.Put<std::int32_t>(0);
        assert(o.Pos() - at == 0x64);
        o.Str(part_name);
        o.Str("sib");
        while (o.Pos() % 4) o.b.push_back(std::byte{0});
    }));
    // Next-table offsets: each table's last list slot points at the following table; recompute.
    // Tables were written back to back, so find their starts by scanning is unnecessary: the
    // starts are list - 12.
    for (size_t t = 0; t + 1 < lists.size(); ++t) {
        const size_t next_start = lists[t + 1].first - 12;
        o.PutAt32(lists[t].first + 4 * lists[t].second, static_cast<std::int32_t>(next_start));
    }
    o.PutAt32(lists.back().first + 4 * lists.back().second, 0);
    return o.b;
}

} // namespace

int main() {
    std::string error;
    {
        auto msb = ParseMsb(Build(Spec{}), &error);
        assert(msb && "valid file parses");
        assert(msb->tables.size() == 4);
        assert(msb->tables[0].name == "MODEL_PARAM_ST" && msb->tables[3].name == "PARTS_PARAM_ST");
        assert(msb->models.size() == 2 && msb->parts.size() == 3);
        assert(msb->models[0].name == "m0000B0A10" && msb->models[1].name == "m0001B0A10");
        assert(msb->models[0].sib_path == "sib" && msb->models[1].instance_count == 1);
        assert(msb->parts[1].name == "part_1" && msb->parts[1].sib_path == "sib" && msb->parts[2].type == 2);
        assert(std::fabs(msb->parts[1].position[0] - 10.0f) < 1e-6f && msb->parts[1].position[2] == -5.0f);
        assert(msb->parts[1].rotation_degrees[1] == 90.0f && msb->parts[1].scale[0] == 1.0f);
        assert(msb->parts[1].draw_groups[0] == 0xFFFFFFFFu && msb->parts[1].disp_groups[0] == 0u);
        const MsbModel* m = msb->ModelOf(msb->parts[1]);
        assert(m && m->name == "m0001B0A10");
        assert(msb->TableCount("EVENT_PARAM_ST") == 0 && msb->TableCount("MISSING") == 0);
        const std::string line = SummarizeMsb(*msb);
        assert(line.find("models 2, parts 3") != std::string::npos);
        assert(line.find("unresolved model refs 0") != std::string::npos);
    }
    { // part naming a model that does not exist: parsed, counted as unresolved
        Spec s; s.bad_model_ref = 7;
        auto msb = ParseMsb(Build(s), &error);
        assert(msb && msb->ModelOf(msb->parts[2]) == nullptr);
        assert(SummarizeMsb(*msb).find("unresolved model refs 1") != std::string::npos);
    }
    { // non-finite position is rejected with a reason
        Spec s; s.nan_position = true;
        assert(!ParseMsb(Build(s), &error));
        assert(error.find("not finite") != std::string::npos);
    }
    { // truncation at any point never crashes and never "succeeds" with a short file
        const Bytes file = Build(Spec{});
        for (size_t cut = 0; cut < file.size(); cut += 7) {
            Bytes shorter(file.begin(), file.begin() + cut);
            assert(!ParseMsb(shorter, &error));
        }
        assert(!ParseMsb({}, &error));
    }
    { // wrong version and implausible count
        Bytes file = Build(Spec{});
        file[0] = std::byte{1};
        assert(!ParseMsb(file, &error) && error.find("version") != std::string::npos);
        file = Build(Spec{});
        const std::int32_t huge = 0x7FFFFFFF;
        std::memcpy(file.data() + 4, &huge, 4);
        assert(!ParseMsb(file, &error) && error.find("implausible") != std::string::npos);
    }
    { // not an MSB at all
        Bytes file(64, std::byte{0x41});
        assert(!ParseMsb(file, &error));
        assert(error.find("first bytes") != std::string::npos);
    }
    std::puts("msb_test: ok");
    return 0;
}
