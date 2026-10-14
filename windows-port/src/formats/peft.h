#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <array>

namespace pact {

struct EffectMotionDescriptor {
    std::uint16_t featureFlags{};
    std::uint8_t framesPerSecond{};
    std::uint16_t frameCount{};
};

struct EffectVertexPrefix {
    std::vector<std::uint16_t> remapIndices;
    std::vector<std::uint32_t> packedEntries;
    std::vector<std::array<std::uint8_t, 3>> packedTriples;
    std::vector<std::uint8_t> auxiliaryBytes;
    struct BaseRecord {
        std::array<std::uint16_t, 6> values{};
        std::uint8_t trackCount{};
    };
    std::vector<BaseRecord> baseRecords;
    std::uint16_t maskSectionFooter{};
    std::uint32_t declaredRemapCapacity{};
    std::uint32_t declaredPackedEntryCapacity{};
    std::uint16_t declaredPackedTripleCapacity{};
    std::uint8_t declaredBaseRecordCount{};
    std::array<std::int32_t, 6> positionBounds{};
    std::vector<std::uint8_t> primaryFrameMasks;
    std::vector<std::uint8_t> secondaryFrameMasks;
    std::size_t consumedBytes{};
};

struct EffectDescriptor {
    std::uint8_t version{};
    std::uint8_t auxiliaryObjectCount{};
    std::uint16_t combinedFeatureFlags{};
    std::vector<EffectMotionDescriptor> motions;
    std::size_t payloadOffset{};
    EffectVertexPrefix vertexPrefix;
};

bool decodePeftV23(std::span<const std::uint8_t> data, EffectDescriptor& effect,
                   std::string& error);

} // namespace pact
