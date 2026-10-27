// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace ember::io {

/// Bounds-checked integer reads from an in-memory byte span. Every read reports failure instead
/// of touching memory outside the span, so parsers can run on untrusted or corrupt files.
class ByteView {
public:
    explicit ByteView(std::span<const std::byte> data, bool big_endian = false) noexcept
        : data_{data}, big_endian_{big_endian} {}

    [[nodiscard]] std::size_t Size() const noexcept {
        return data_.size();
    }
    void SetBigEndian(bool big_endian) noexcept {
        big_endian_ = big_endian;
    }
    [[nodiscard]] bool BigEndian() const noexcept {
        return big_endian_;
    }

    template <typename T>
        requires std::is_integral_v<T>
    [[nodiscard]] bool Read(std::uint64_t offset, T& out) const noexcept {
        if (offset > data_.size() || data_.size() - offset < sizeof(T)) {
            return false;
        }
        T value;
        std::memcpy(&value, data_.data() + offset, sizeof(T));
        const bool host_big = std::endian::native == std::endian::big;
        if (big_endian_ != host_big) {
            value = ByteSwap(value);
        }
        out = value;
        return true;
    }

    [[nodiscard]] bool Matches(std::uint64_t offset, const char* ascii,
                               std::size_t length) const noexcept {
        if (offset > data_.size() || data_.size() - offset < length) {
            return false;
        }
        return std::memcmp(data_.data() + offset, ascii, length) == 0;
    }

private:
    template <typename T>
    static T ByteSwap(T value) noexcept {
        auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
        for (std::size_t i = 0; i < sizeof(T) / 2; ++i) {
            std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
        }
        return std::bit_cast<T>(bytes);
    }

    std::span<const std::byte> data_;
    bool big_endian_;
};

} // namespace ember::io
