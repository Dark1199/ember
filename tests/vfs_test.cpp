// SPDX-License-Identifier: MIT
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "vfs/vfs.h"

namespace fs = std::filesystem;

static void Write(const fs::path& path, const char* text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

int main() {
    using ember::vfs::Vfs;
    const fs::path base = fs::temp_directory_path() / "ember_vfs_test";
    fs::remove_all(base);
    Write(base / "a" / "Data" / "Map" / "Tile01.BIN", "base");
    Write(base / "a" / "readme.txt", "r");
    Write(base / "b" / "data" / "map" / "tile01.bin", "patched");

    assert(Vfs::Normalize("\\Data\\\\Map/./Tile01.BIN") == "data/map/tile01.bin");

    Vfs vfs;
    std::string error;
    assert(vfs.MountDirectory(base / "a", &error));
    assert(vfs.FileCount() == 2);
    assert(vfs.Exists("DATA\\MAP\\TILE01.bin"));
    assert(!vfs.Exists("data/map/missing.bin"));
    assert(vfs.ReadAll("data/map/tile01.bin").value().size() == 4);

    // A later mount overrides an earlier one with the same case-folded path.
    assert(vfs.MountDirectory(base / "b", &error));
    assert(vfs.FileCount() == 2);
    assert(vfs.ReadAll("Data/Map/Tile01.BIN").value().size() == 7);

    const auto top = vfs.TopLevel();
    assert(top.size() == 2 && top[0] == "data" && top[1] == "readme.txt");
    assert(vfs.List("data/map").size() == 1);

    assert(!vfs.MountDirectory(base / "does_not_exist", &error));
    assert(!error.empty());

    fs::remove_all(base);
    std::puts("vfs_test: ok");
    return 0;
}
