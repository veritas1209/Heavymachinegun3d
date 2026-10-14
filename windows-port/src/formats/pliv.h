#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "formats/pmod.h"

namespace pact {

constexpr std::uint16_t kPlivBoneAnimation = 0x0100U;
constexpr std::uint16_t kPlivFlowedTextureAnimation = 0x0200U;
constexpr std::uint16_t kPlivUnflowedTextureAnimation = 0x0400U;
constexpr std::uint16_t kPlivVertexAnimation = 0x1000U;

struct MotionDescriptor {
    std::uint16_t featureFlags{};
    std::uint8_t attachmentTrackCount{};
    std::uint8_t framesPerSecond{};
    std::uint8_t sampleCount{};
    std::uint16_t frameCount{};
};

struct ActionDescriptor {
    std::uint8_t version{};
    std::uint8_t auxiliaryObjectCount{};
    std::uint16_t combinedFeatureFlags{};
    std::vector<MotionDescriptor> motions;
    std::size_t payloadOffset{};
};

struct UvKey {
    std::uint16_t u{};
    std::uint16_t v{};
};

struct UvTrack {
    std::uint8_t textureUnit{};
    std::vector<std::uint16_t> frameToKey;
    std::vector<UvKey> keys;
};

struct UnflowedTextureMotion {
    std::size_t motionIndex{};
    std::vector<UvTrack> tracks;
};

struct UnflowedTextureSection {
    std::uint16_t declaredKeyCount{};
    std::vector<UnflowedTextureMotion> motions;
    std::size_t consumedBytes{};
};

struct BoneDescriptor {
    std::array<std::uint8_t, 5> identifiers{};
    std::uint16_t rawField{};
    std::array<std::int16_t, 4> bindRotation{};
    std::array<std::uint8_t, 3> bindPosition{};
};

struct BoneSectionHeader {
    std::uint8_t boneCount{};
    std::uint16_t rawFrameTableCapacity{};
    std::uint16_t rotationCacheCapacity{};
    std::uint16_t positionCacheCapacity{};
    std::array<std::int32_t, 3> positionMinimum{};
    std::array<std::int32_t, 3> positionMaximum{};
    std::vector<BoneDescriptor> bones;
    std::size_t descriptorBytes{};
    std::size_t trackDataOffset{};
};

struct QuaternionKey {
    std::array<float, 4> value{};
};

struct PositionKey {
    std::array<std::int32_t, 3> value{};
};

struct BoneTrack {
    std::vector<std::uint16_t> frameToRotationKey;
    std::vector<std::uint16_t> frameToPositionKey;
    std::vector<QuaternionKey> rotationKeys;
    std::vector<PositionKey> positionKeys;
};

struct BoneMotion {
    std::size_t motionIndex{};
    std::vector<BoneTrack> tracks;
};

struct BoneSection {
    BoneSectionHeader header;
    std::vector<BoneMotion> motions;
    std::size_t storedRotationKeyCount{};
    std::size_t storedPositionKeyCount{};
    std::size_t consumedBytes{};
};

struct VertexSectionPrefix {
    std::vector<std::uint16_t> remapIndices;
    std::vector<std::uint32_t> packedEntries;
    std::vector<std::array<std::uint8_t, 3>> packedTriples;
    std::uint16_t reservedCount{};
    std::uint16_t materialRecordCount{};
    std::size_t vertexDataOffset{};
    std::size_t afterVertexDataOffset{};
};

struct VertexMaterialRecord {
    std::array<std::uint8_t, 13> raw{};
};

struct VertexAnimationMotion {
    std::size_t motionIndex{};
    std::uint16_t remapOffset{};
    std::uint16_t remapCount{};
    std::uint16_t texcoordOffset{};
    std::uint16_t texcoordCount{};
    std::uint16_t packedPositionOffset{};
    std::uint16_t packedPositionCount{};
    std::uint8_t attachmentTrackCount{};
    std::array<std::int32_t, 3> positionMinimum{};
    std::array<std::int32_t, 3> positionMaximum{};
    std::vector<std::vector<Vec3>> frames;
};

struct VertexAnimationSection {
    std::vector<std::uint16_t> remapIndices;
    std::vector<Vec2> texcoords;
    std::vector<VertexMaterialRecord> materials;
    std::vector<VertexAnimationMotion> motions;
    std::uint16_t frameMaskByteCount{};
    std::vector<std::uint8_t> frameMask;
    std::vector<std::uint8_t> trailingData;
    std::size_t consumedBytes{};
};

bool decodePlivV23Header(std::span<const std::uint8_t> data, ActionDescriptor& action,
                         std::string& error);
bool decodePlivV23UnflowedTexture(std::span<const std::uint8_t> data,
                                 const ActionDescriptor& action, std::size_t offset,
                                 UnflowedTextureSection& section, std::string& error);
bool decodePlivV23BoneHeader(std::span<const std::uint8_t> data, std::size_t offset,
                            BoneSectionHeader& section, std::string& error);
bool decodePlivV23Bone(std::span<const std::uint8_t> data, const ActionDescriptor& action,
                       std::size_t offset, BoneSection& section, std::string& error);
bool decodePlivV23VertexPrefix(std::span<const std::uint8_t> data, std::size_t offset,
                              VertexSectionPrefix& section, std::string& error);
bool decodePlivV23Vertex(std::span<const std::uint8_t> data,
                        const ActionDescriptor& action, std::size_t offset,
                        VertexAnimationSection& section, std::string& error);

} // namespace pact
