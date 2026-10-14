#include "formats/pcam.h"

#include <algorithm>
#include <cstring>

namespace pact {
namespace {

std::uint16_t readLe16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
        static_cast<std::uint16_t>(data[offset + 1] << 8U);
}

float readFixed16_16(std::span<const std::uint8_t> data, std::size_t offset) {
    std::int32_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return static_cast<float>(value) / 65536.0f;
}

bool tagged(std::span<const std::uint8_t> mask, std::size_t frame) {
    return (mask[frame >> 3U] & (1U << (frame & 7U))) != 0;
}

bool decodeScalarTrack(std::span<const std::uint8_t> data, std::size_t& cursor,
                       std::uint16_t frameCount, bool animated,
                       std::vector<float>& values, std::string& error) {
    const std::size_t maskBytes = static_cast<std::size_t>(frameCount >> 3U) + 1U;
    if (cursor + maskBytes > data.size()) {
        error = "truncated scalar frame mask";
        return false;
    }
    const auto mask = data.subspan(cursor, maskBytes);
    cursor += maskBytes;
    values.assign(frameCount, 0.0f);
    if (!animated) {
        if (cursor + 4U > data.size()) {
            error = "truncated constant scalar";
            return false;
        }
        const float value = readFixed16_16(data, cursor);
        cursor += 4U;
        std::fill(values.begin(), values.end(), value);
        return true;
    }
    float current = 0.0f;
    bool haveValue = false;
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        if (tagged(mask, frame)) {
            if (cursor + 4U > data.size()) {
                error = "truncated animated scalar";
                return false;
            }
            current = readFixed16_16(data, cursor);
            cursor += 4U;
            haveValue = true;
        }
        if (!haveValue) {
            error = "animated scalar has no initial key";
            return false;
        }
        values[frame] = current;
    }
    return true;
}

bool decodeVectorTrack(std::span<const std::uint8_t> data, std::size_t& cursor,
                       std::uint16_t frameCount, bool animated,
                       std::vector<Vec3>& values, std::string& error) {
    const std::size_t maskBytes = static_cast<std::size_t>(frameCount >> 3U) + 1U;
    if (cursor + maskBytes > data.size()) {
        error = "truncated vector frame mask";
        return false;
    }
    const auto mask = data.subspan(cursor, maskBytes);
    cursor += maskBytes;
    values.assign(frameCount, Vec3{});
    if (!animated) {
        if (cursor + 12U > data.size()) {
            error = "truncated constant vector";
            return false;
        }
        const Vec3 value{readFixed16_16(data, cursor), readFixed16_16(data, cursor + 4U),
                         readFixed16_16(data, cursor + 8U)};
        cursor += 12U;
        std::fill(values.begin(), values.end(), value);
        return true;
    }
    Vec3 current{};
    bool haveValue = false;
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        if (tagged(mask, frame)) {
            if (cursor + 12U > data.size()) {
                error = "truncated animated vector";
                return false;
            }
            current = Vec3{readFixed16_16(data, cursor), readFixed16_16(data, cursor + 4U),
                           readFixed16_16(data, cursor + 8U)};
            cursor += 12U;
            haveValue = true;
        }
        if (!haveValue) {
            error = "animated vector has no initial key";
            return false;
        }
        values[frame] = current;
    }
    return true;
}

} // namespace

bool decodePcamV23(std::span<const std::uint8_t> data, CameraTrack& camera,
                   std::string& error) {
    camera = CameraTrack{};
    if (data.size() < 66U || data[0] != 'P' || data[1] != 'C' || data[2] != 'A') {
        error = "not a PCAM payload";
        return false;
    }
    camera.version = data[3];
    if (camera.version != 23U) {
        error = "unsupported PCAM version";
        return false;
    }
    const std::uint8_t cameraCount = data[4];
    if (cameraCount != 1U) {
        error = "only one-camera PCAM v23 files are supported";
        return false;
    }
    camera.frameCount = readLe16(data, 5);
    camera.framesPerSecond = data[7];
    if (camera.frameCount == 0U || camera.framesPerSecond == 0U) {
        error = "invalid PCAM timing";
        return false;
    }
    camera.flags = data[52];
    std::size_t cursor = 52U + cameraCount;
    camera.cameraId = data[cursor];
    cursor += 13U;

    std::vector<Vec3> unusedVector;
    if ((camera.flags & 0x01U) != 0U &&
        !decodeVectorTrack(data, cursor, camera.frameCount, true, unusedVector, error)) {
        return false;
    }
    if ((camera.flags & 0x02U) != 0U &&
        !decodeVectorTrack(data, cursor, camera.frameCount, true, unusedVector, error)) {
        return false;
    }

    std::vector<float> fieldOfView;
    std::vector<float> roll;
    if (!decodeScalarTrack(data, cursor, camera.frameCount,
                           (camera.flags & 0x10U) != 0U, fieldOfView, error) ||
        !decodeScalarTrack(data, cursor, camera.frameCount,
                           (camera.flags & 0x20U) != 0U, roll, error) ||
        !decodeVectorTrack(data, cursor, camera.frameCount,
                           (camera.flags & 0x40U) != 0U, camera.positions, error) ||
        !decodeVectorTrack(data, cursor, camera.frameCount,
                           (camera.flags & 0x80U) != 0U, camera.targets, error)) {
        return false;
    }
    camera.fieldOfViewDegrees = fieldOfView.front();
    camera.rollDegrees = roll.front();
    camera.consumedBytes = cursor;
    if (cursor != data.size()) {
        error = "unexpected PCAM trailing bytes";
        return false;
    }
    return true;
}

} // namespace pact
