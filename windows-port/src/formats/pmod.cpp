#include "formats/pmod.h"

#include <cmath>

namespace pact {
namespace {

std::uint16_t readU16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
        static_cast<std::uint16_t>(data[offset + 1]) << 8U;
}

std::int32_t readI32(std::span<const std::uint8_t> data, std::size_t offset) {
    const std::uint32_t value = static_cast<std::uint32_t>(data[offset]) |
        static_cast<std::uint32_t>(data[offset + 1]) << 8U |
        static_cast<std::uint32_t>(data[offset + 2]) << 16U |
        static_cast<std::uint32_t>(data[offset + 3]) << 24U;
    return static_cast<std::int32_t>(value);
}

float fixed16ToFloat(std::int32_t value) {
    return static_cast<float>(value) / 65536.0f;
}

float decodePosition(std::uint8_t quantized, std::int32_t minimum, std::int32_t maximum) {
    const std::int64_t range = static_cast<std::int64_t>(maximum) - minimum;
    const std::int64_t fixed = static_cast<std::int64_t>(minimum) +
        ((range >> 8) * static_cast<std::int64_t>(quantized));
    return static_cast<float>(fixed) / 65536.0f;
}

} // namespace

bool decodePmodV23(std::span<const std::uint8_t> data, Mesh& mesh, std::string& error) {
    mesh = {};
    if (data.size() < 65) {
        error = "PMOD is shorter than the version-23 header";
        return false;
    }
    if (data[0] != 'P' || data[1] != 'M' || data[2] != 'M') {
        error = "PMOD magic is not PMM";
        return false;
    }

    mesh.version = data[3];
    mesh.flags = data[4];
    mesh.attributeCount = data[5];
    if (mesh.version != 23) {
        error = "only PMOD version 23 is implemented";
        return false;
    }
    if ((mesh.flags & 0x1FU) != 0x1FU) {
        error = "version-23 mesh is missing expected position/normal/UV/index flags";
        return false;
    }

    const std::size_t indexCount = readU16(data, 6);
    const std::size_t vertexCount = readU16(data, 8);
    const std::size_t payloadOffset = 56U + static_cast<std::size_t>(mesh.attributeCount) * 9U;
    const std::size_t positionBytes = vertexCount * 3U;
    const std::size_t normalBytes = vertexCount * 3U;
    const std::size_t texcoordBytes = vertexCount * 4U;
    const std::size_t indexBytes = indexCount * 2U;
    const std::size_t required = payloadOffset + positionBytes + normalBytes + texcoordBytes + indexBytes;
    if (payloadOffset < 56U || required < payloadOffset || required > data.size()) {
        error = "PMOD payload ranges exceed the file";
        return false;
    }
    if (vertexCount == 0 || indexCount == 0 || indexCount % 3U != 0U) {
        error = "PMOD has an invalid vertex or triangle index count";
        return false;
    }

    std::array<std::int32_t, 6> fixedBounds{};
    for (std::size_t index = 0; index < fixedBounds.size(); ++index) {
        fixedBounds[index] = readI32(data, 32U + index * 4U);
    }
    mesh.bounds[0] = {fixed16ToFloat(fixedBounds[0]), fixed16ToFloat(fixedBounds[1]), fixed16ToFloat(fixedBounds[2])};
    mesh.bounds[1] = {fixed16ToFloat(fixedBounds[3]), fixed16ToFloat(fixedBounds[4]), fixed16ToFloat(fixedBounds[5])};

    mesh.positions.reserve(vertexCount);
    mesh.normals.reserve(vertexCount);
    mesh.texcoords.reserve(vertexCount);
    mesh.indices.reserve(indexCount);
    std::size_t cursor = payloadOffset;
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
        mesh.positions.push_back({
            decodePosition(data[cursor], fixedBounds[0], fixedBounds[3]),
            decodePosition(data[cursor + 1], fixedBounds[1], fixedBounds[4]),
            decodePosition(data[cursor + 2], fixedBounds[2], fixedBounds[5]),
        });
        cursor += 3;
    }
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
        mesh.normals.push_back({
            static_cast<float>(static_cast<std::int8_t>(data[cursor])) / 128.0f,
            static_cast<float>(static_cast<std::int8_t>(data[cursor + 1])) / 128.0f,
            static_cast<float>(static_cast<std::int8_t>(data[cursor + 2])) / 128.0f,
        });
        cursor += 3;
    }
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
        mesh.texcoords.push_back({
            static_cast<float>(readU16(data, cursor)) / 256.0f,
            static_cast<float>(readU16(data, cursor + 2)) / 256.0f,
        });
        cursor += 4;
    }
    for (std::size_t index = 0; index < indexCount; ++index) {
        const auto value = readU16(data, cursor);
        if (value >= vertexCount) {
            error = "PMOD triangle references a vertex outside the vertex array";
            return false;
        }
        mesh.indices.push_back(value);
        cursor += 2;
    }

    for (const Vec3& position : mesh.positions) {
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
            error = "PMOD produced a non-finite position";
            return false;
        }
    }
    mesh.consumedBytes = cursor;
    error.clear();
    return true;
}

} // namespace pact
