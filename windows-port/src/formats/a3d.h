#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pact {

struct A3dField {
    std::uint8_t type{};
    std::uint8_t size{};
    std::uint16_t offset{};
};

struct A3dTable {
    std::uint16_t headerSize{};
    std::uint16_t rowSize{};
    std::uint16_t rowCount{};
    std::vector<A3dField> fields;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] std::int32_t signedValue(
        std::size_t row, std::size_t field, std::int32_t fallback = 0) const;
    [[nodiscard]] std::uint32_t unsignedValue(
        std::size_t row, std::size_t field, std::uint32_t fallback = 0) const;
    [[nodiscard]] std::string stringValue(
        std::size_t row, std::size_t field) const;
};

bool decodeA3d(std::span<const std::uint8_t> data, A3dTable& table, std::string& error);

} // namespace pact
