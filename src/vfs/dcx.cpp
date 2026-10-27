// SPDX-License-Identifier: GPL-3.0-or-later
#include "vfs/dcx.h"

#include <zlib.h>

#include <limits>

#include "io/bytes.h"

namespace ember::vfs {

namespace {

bool Fail(std::string* error, const std::string& what) {
    if (error) {
        *error = what;
    }
    return false;
}

} // namespace

bool IsDcx(std::span<const std::byte> bytes) noexcept {
    return io::ByteView(bytes).Matches(0, "DCX\0", 4);
}

std::optional<std::vector<std::byte>> InflateZlib(std::span<const std::byte> in,
                                                  std::uint64_t expected_size,
                                                  std::string* error) {
    if (expected_size > kMaxDecompressedSize) {
        Fail(error, "declared size " + std::to_string(expected_size) + " exceeds the safety limit");
        return std::nullopt;
    }
    if (in.size() > std::numeric_limits<uInt>::max()) {
        Fail(error, "compressed input too large for one zlib call");
        return std::nullopt;
    }
    std::vector<std::byte> out(static_cast<std::size_t>(expected_size));

    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        Fail(error, "inflateInit failed");
        return std::nullopt;
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
    stream.avail_in = static_cast<uInt>(in.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());

    const int result = inflate(&stream, Z_FINISH);
    const uLong produced = stream.total_out;
    inflateEnd(&stream);

    if (result != Z_STREAM_END) {
        Fail(error, std::string("zlib stream is corrupt or does not fit the declared size (zlib ") +
                        std::to_string(result) + ")");
        return std::nullopt;
    }
    if (produced != out.size()) {
        Fail(error, "decompressed " + std::to_string(produced) + " bytes, header declared " +
                        std::to_string(out.size()));
        return std::nullopt;
    }
    return out;
}

std::optional<std::vector<std::byte>> DecompressDcx(std::span<const std::byte> bytes,
                                                    std::string* error) {
    io::ByteView view(bytes, /*big_endian=*/true);
    if (!IsDcx(bytes)) {
        Fail(error, "missing DCX magic");
        return std::nullopt;
    }
    std::uint32_t dcs_offset = 0;
    std::uint32_t dcp_offset = 0;
    if (!view.Read(8, dcs_offset) || !view.Read(12, dcp_offset)) {
        Fail(error, "truncated DCX header");
        return std::nullopt;
    }

    std::uint32_t uncompressed = 0;
    std::uint32_t compressed = 0;
    if (!view.Matches(dcs_offset, "DCS\0", 4) || !view.Read(std::uint64_t{dcs_offset} + 4, uncompressed) ||
        !view.Read(std::uint64_t{dcs_offset} + 8, compressed)) {
        Fail(error, "DCS block missing or truncated");
        return std::nullopt;
    }

    if (!view.Matches(dcp_offset, "DCP\0", 4) || !view.Matches(std::uint64_t{dcp_offset} + 4, "DFLT", 4)) {
        char method[5] = {};
        for (int i = 0; i < 4; ++i) {
            std::uint8_t c = 0;
            if (view.Read(std::uint64_t{dcp_offset} + 4 + i, c)) {
                method[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
            }
        }
        Fail(error, std::string("unsupported compression method '") + method + "'");
        return std::nullopt;
    }
    std::uint32_t dcp_size = 0;
    if (!view.Read(std::uint64_t{dcp_offset} + 8, dcp_size)) {
        Fail(error, "DCP block truncated");
        return std::nullopt;
    }
    const std::uint64_t dca_offset = std::uint64_t{dcp_offset} + dcp_size;
    std::uint32_t dca_size = 0;
    if (!view.Matches(dca_offset, "DCA\0", 4) || !view.Read(dca_offset + 4, dca_size)) {
        Fail(error, "DCA block missing or truncated");
        return std::nullopt;
    }
    const std::uint64_t data_offset = dca_offset + dca_size;
    if (data_offset > bytes.size() || bytes.size() - data_offset < compressed) {
        Fail(error, "compressed data lies outside the file");
        return std::nullopt;
    }
    return InflateZlib(bytes.subspan(static_cast<std::size_t>(data_offset), compressed),
                       uncompressed, error);
}

} // namespace ember::vfs
