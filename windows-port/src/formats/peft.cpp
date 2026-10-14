#include "formats/peft.h"

#include <limits>

namespace pact {
namespace {

std::uint16_t readLe16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
        static_cast<std::uint16_t>(data[offset + 1U] << 8U);
}

std::uint32_t readLe32(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
        (static_cast<std::uint32_t>(data[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(data[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(data[offset + 3U]) << 24U);
}

std::int32_t readLe32Signed(std::span<const std::uint8_t> data,
                            std::size_t offset) {
    return static_cast<std::int32_t>(readLe32(data, offset));
}

} // namespace

bool decodePeftV23(std::span<const std::uint8_t> data, EffectDescriptor& effect,
                   std::string& error) {
    effect = EffectDescriptor{};
    if (data.size() < 30U) {
        error = "truncated PEFT v23 header";
        return false;
    }
    if (data[0] != 'P' || data[1] != 'E' || data[2] != 'M') {
        error = "invalid PEFT signature";
        return false;
    }
    effect.version = data[3];
    if (effect.version != 23U) {
        error = "unsupported PEFT version " + std::to_string(effect.version);
        return false;
    }
    const std::size_t motionCount = data[4];
    if (motionCount == 0U) {
        error = "PEFT contains no motions";
        return false;
    }
    effect.auxiliaryObjectCount = data[5];

    // pactLoadEffectEx skips the 19-byte version-23 common block at offset 6,
    // then consumes one five-byte descriptor per motion.
    constexpr std::size_t motionTableOffset = 25U;
    constexpr std::size_t motionStride = 5U;
    if (motionCount > (std::numeric_limits<std::size_t>::max() -
                       motionTableOffset) / motionStride ||
        motionTableOffset + motionCount * motionStride > data.size()) {
        error = "truncated PEFT motion table";
        return false;
    }
    effect.motions.reserve(motionCount);
    for (std::size_t index = 0; index < motionCount; ++index) {
        const std::size_t offset = motionTableOffset + index * motionStride;
        EffectMotionDescriptor motion;
        motion.featureFlags = readLe16(data, offset);
        motion.framesPerSecond = data[offset + 2U];
        motion.frameCount = readLe16(data, offset + 3U);
        if (motion.framesPerSecond == 0U || motion.frameCount == 0U) {
            error = "invalid PEFT motion timing at index " + std::to_string(index);
            return false;
        }
        effect.combinedFeatureFlags |= motion.featureFlags;
        effect.motions.push_back(motion);
    }
    effect.payloadOffset = motionTableOffset + motionCount * motionStride;
    // _pactLoadAniVertex shares its counted-table prefix with PLIV.  PEFT then
    // carries its own auxiliary/base records, repeated capacities and bounds,
    // and primary/optional secondary frame-mask streams.
    if (effect.payloadOffset + 6U > data.size()) {
        error = "truncated PEFT vertex prefix";
        return false;
    }
    std::size_t cursor = effect.payloadOffset + 4U;
    const auto takeCounted = [&](std::size_t stride, std::size_t& count) {
        if (cursor + 2U > data.size()) return false;
        count = readLe16(data, cursor);
        cursor += 2U;
        return count <= (data.size() - cursor) / stride;
    };
    std::size_t count = 0;
    if (!takeCounted(2U, count)) {
        error = "truncated PEFT vertex remap table";
        return false;
    }
    effect.vertexPrefix.remapIndices.reserve(count);
    for (std::size_t index = 0; index < count; ++index, cursor += 2U) {
        effect.vertexPrefix.remapIndices.push_back(readLe16(data, cursor));
    }
    if (!takeCounted(4U, count)) {
        error = "truncated PEFT packed-entry table";
        return false;
    }
    effect.vertexPrefix.packedEntries.reserve(count);
    for (std::size_t index = 0; index < count; ++index, cursor += 4U) {
        effect.vertexPrefix.packedEntries.push_back(readLe32(data, cursor));
    }
    if (!takeCounted(3U, count)) {
        error = "truncated PEFT packed-triple table";
        return false;
    }
    effect.vertexPrefix.packedTriples.reserve(count);
    for (std::size_t index = 0; index < count; ++index, cursor += 3U) {
        effect.vertexPrefix.packedTriples.push_back(
            {data[cursor], data[cursor + 1U], data[cursor + 2U]});
    }
    if (cursor + 2U > data.size()) {
        error = "truncated PEFT auxiliary-byte count";
        return false;
    }
    const std::size_t auxiliaryByteCount = readLe16(data, cursor);
    cursor += 2U;
    if (auxiliaryByteCount > data.size() - cursor) {
        error = "truncated PEFT auxiliary-byte block";
        return false;
    }
    effect.vertexPrefix.auxiliaryBytes.assign(
        data.begin() + static_cast<std::ptrdiff_t>(cursor),
        data.begin() + static_cast<std::ptrdiff_t>(cursor + auxiliaryByteCount));
    cursor += auxiliaryByteCount;
    if (cursor + 2U > data.size()) {
        error = "truncated PEFT base-record count";
        return false;
    }
    const std::size_t baseRecordCount = readLe16(data, cursor);
    cursor += 2U;
    constexpr std::size_t baseRecordStrideV23 = 13U;
    if (baseRecordCount > (data.size() - cursor) / baseRecordStrideV23) {
        error = "truncated PEFT v23 base-record table";
        return false;
    }
    effect.vertexPrefix.baseRecords.reserve(baseRecordCount);
    for (std::size_t recordIndex = 0; recordIndex < baseRecordCount;
         ++recordIndex, cursor += baseRecordStrideV23) {
        EffectVertexPrefix::BaseRecord record;
        for (std::size_t field = 0; field < record.values.size(); ++field) {
            record.values[field] = readLe16(data, cursor + field * 2U);
        }
        record.trackCount = data[cursor + 12U];
        effect.vertexPrefix.baseRecords.push_back(record);
    }
    // The final byte is a per-record embedded track count.  All shipped v23
    // PEFT records use zero, so reject an unverified inline-track variant.
    for (const auto& record : effect.vertexPrefix.baseRecords) {
        if (record.trackCount != 0U) {
            error = "unsupported PEFT base-record frame masks";
            return false;
        }
    }
    if (cursor + 2U > data.size()) {
        error = "truncated PEFT mask-section footer";
        return false;
    }
    effect.vertexPrefix.maskSectionFooter = readLe16(data, cursor);
    cursor += 2U;
    if (cursor + 35U > data.size()) {
        error = "truncated PEFT capacity and bounds block";
        return false;
    }
    effect.vertexPrefix.declaredRemapCapacity = readLe32(data, cursor);
    effect.vertexPrefix.declaredPackedEntryCapacity = readLe32(data, cursor + 4U);
    effect.vertexPrefix.declaredPackedTripleCapacity = readLe16(data, cursor + 8U);
    effect.vertexPrefix.declaredBaseRecordCount = data[cursor + 10U];
    for (std::size_t index = 0; index < effect.vertexPrefix.positionBounds.size(); ++index) {
        effect.vertexPrefix.positionBounds[index] =
            readLe32Signed(data, cursor + 11U + index * 4U);
    }
    cursor += 35U;
    if (effect.vertexPrefix.declaredRemapCapacity !=
            effect.vertexPrefix.remapIndices.size() ||
        effect.vertexPrefix.declaredPackedEntryCapacity !=
            effect.vertexPrefix.packedEntries.size() ||
        effect.vertexPrefix.declaredPackedTripleCapacity !=
            effect.vertexPrefix.packedTriples.size() ||
        effect.vertexPrefix.declaredBaseRecordCount !=
            effect.vertexPrefix.baseRecords.size()) {
        error = "PEFT repeated capacity block does not match decoded tables";
        return false;
    }
    const std::size_t maskBytesPerRecord =
        static_cast<std::size_t>(effect.motions.front().frameCount) / 8U + 1U;
    const std::size_t expectedMaskBytes =
        maskBytesPerRecord * effect.vertexPrefix.baseRecords.size();
    const auto readMaskSection = [&](std::vector<std::uint8_t>& masks,
                                     const char* label, bool required) {
        if (cursor + 2U > data.size()) {
            error = std::string("truncated PEFT ") + label + " mask count";
            return false;
        }
        const std::size_t byteCount = readLe16(data, cursor);
        cursor += 2U;
        // The primary stream covers every base record.  The optional secondary
        // stream covers only records that carry a second animated component;
        // bossLaser is the shipped example with two masks for three records.
        const bool invalidRequired = required && byteCount != expectedMaskBytes;
        const bool invalidOptional = !required &&
            (byteCount > expectedMaskBytes ||
             byteCount % maskBytesPerRecord != 0U);
        if (invalidRequired || invalidOptional) {
            error = std::string("unexpected PEFT ") + label + " mask byte count";
            return false;
        }
        if (byteCount > data.size() - cursor) {
            error = std::string("truncated PEFT ") + label + " masks";
            return false;
        }
        masks.assign(data.begin() + static_cast<std::ptrdiff_t>(cursor),
                     data.begin() + static_cast<std::ptrdiff_t>(cursor + byteCount));
        cursor += byteCount;
        return true;
    };
    if (!readMaskSection(effect.vertexPrefix.primaryFrameMasks,
                         "primary-frame", true) ||
        !readMaskSection(effect.vertexPrefix.secondaryFrameMasks,
                         "secondary-frame", false)) {
        return false;
    }
    effect.vertexPrefix.consumedBytes = cursor;
    if (cursor != data.size()) {
        error = "unconsumed PEFT v23 bytes: " +
            std::to_string(data.size() - cursor);
        return false;
    }
    return true;
}

} // namespace pact
