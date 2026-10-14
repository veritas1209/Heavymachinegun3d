#include "formats/a3d.h"

#include <algorithm>
#include <limits>

namespace pact {
namespace {

std::uint16_t readBe16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) << 8U |
        static_cast<std::uint16_t>(data[offset + 1U]);
}

std::uint16_t readLe16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
        static_cast<std::uint16_t>(data[offset + 1U]) << 8U;
}

std::uint32_t readLe32(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
        static_cast<std::uint32_t>(data[offset + 1U]) << 8U |
        static_cast<std::uint32_t>(data[offset + 2U]) << 16U |
        static_cast<std::uint32_t>(data[offset + 3U]) << 24U;
}

bool fieldRange(const A3dTable& table, std::size_t row, std::size_t field,
                std::size_t& offset, std::size_t& size) {
    if (row >= table.rowCount || field >= table.fields.size()) {
        return false;
    }
    const A3dField& descriptor = table.fields[field];
    offset = static_cast<std::size_t>(table.headerSize) +
        row * static_cast<std::size_t>(table.rowSize) + descriptor.offset;
    size = descriptor.size;
    return size != 0U && offset <= table.bytes.size() &&
        size <= table.bytes.size() - offset;
}

} // namespace

std::int32_t A3dTable::signedValue(
        std::size_t row, std::size_t field, std::int32_t fallback) const {
    std::size_t offset = 0;
    std::size_t size = 0;
    if (!fieldRange(*this, row, field, offset, size)) {
        return fallback;
    }
    if (size == 1U) {
        return static_cast<std::int8_t>(bytes[offset]);
    }
    if (size == 2U) {
        return static_cast<std::int16_t>(readLe16(bytes, offset));
    }
    if (size == 4U) {
        return static_cast<std::int32_t>(readLe32(bytes, offset));
    }
    return fallback;
}

std::uint32_t A3dTable::unsignedValue(
        std::size_t row, std::size_t field, std::uint32_t fallback) const {
    std::size_t offset = 0;
    std::size_t size = 0;
    if (!fieldRange(*this, row, field, offset, size)) {
        return fallback;
    }
    if (size == 1U) {
        return bytes[offset];
    }
    if (size == 2U) {
        return readLe16(bytes, offset);
    }
    if (size == 4U) {
        return readLe32(bytes, offset);
    }
    return fallback;
}

std::string A3dTable::stringValue(std::size_t row, std::size_t field) const {
    std::size_t offset = 0;
    std::size_t size = 0;
    if (!fieldRange(*this, row, field, offset, size)) {
        return {};
    }
    const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(size);
    const auto terminator = std::find(begin, end, static_cast<std::uint8_t>(0));
    return std::string(begin, terminator);
}

bool decodeA3d(std::span<const std::uint8_t> data,
               A3dTable& table, std::string& error) {
    table = {};
    if (data.size() < 12U) {
        error = "A3D is shorter than its schema header";
        return false;
    }
    const std::uint16_t headerSize = readBe16(data, 0U);
    const std::uint16_t rowSize = readBe16(data, 2U);
    const std::uint16_t rowCount = readBe16(data, 4U);
    const std::uint16_t fieldCount = readBe16(data, 6U);
    const std::size_t descriptorEnd = 8U +
        static_cast<std::size_t>(fieldCount) * 4U;
    if (fieldCount == 0U || rowCount == 0U || rowSize == 0U ||
        headerSize < descriptorEnd || headerSize > data.size()) {
        error = "A3D header sizes or counts are invalid";
        return false;
    }

    table.headerSize = headerSize;
    table.rowSize = rowSize;
    table.rowCount = rowCount;
    table.fields.reserve(fieldCount);
    std::size_t maximumFieldEnd = 0U;
    for (std::size_t index = 0; index < fieldCount; ++index) {
        const std::size_t descriptor = 8U + index * 4U;
        A3dField field{
            data[descriptor],
            data[descriptor + 1U],
            readLe16(data, descriptor + 2U),
        };
        const std::size_t fieldEnd =
            static_cast<std::size_t>(field.offset) + field.size;
        if (field.size == 0U || fieldEnd > rowSize) {
            error = "A3D field exceeds the declared row size";
            table = {};
            return false;
        }
        maximumFieldEnd = std::max(maximumFieldEnd, fieldEnd);
        table.fields.push_back(field);
    }
    const std::size_t lastRowOffset = static_cast<std::size_t>(headerSize) +
        static_cast<std::size_t>(rowCount - 1U) * rowSize;
    if (lastRowOffset > data.size() ||
        maximumFieldEnd > data.size() - lastRowOffset) {
        error = "A3D record fields exceed the file";
        table = {};
        return false;
    }
    table.bytes.assign(data.begin(), data.end());
    error.clear();
    return true;
}

} // namespace pact
