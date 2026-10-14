#include "formats/pliv.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pact {
namespace {

std::uint16_t readLe16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
        static_cast<std::uint16_t>(data[offset + 1] << 8U);
}

std::int32_t readLe32Signed(std::span<const std::uint8_t> data, std::size_t offset) {
    const std::uint32_t value = static_cast<std::uint32_t>(data[offset]) |
        (static_cast<std::uint32_t>(data[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(data[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(data[offset + 3U]) << 24U);
    return static_cast<std::int32_t>(value);
}

std::int16_t readLe16Signed(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::int16_t>(readLe16(data, offset));
}

QuaternionKey readQuaternion(std::span<const std::uint8_t> data, std::size_t offset) {
    QuaternionKey key;
    float lengthSquared = 0.0F;
    for (std::size_t component = 0; component < key.value.size(); ++component) {
        key.value[component] = static_cast<float>(readLe16Signed(data, offset + component * 2U));
        lengthSquared += key.value[component] * key.value[component];
    }
    if (lengthSquared > 0.0F) {
        const float inverseLength = 1.0F / std::sqrt(lengthSquared);
        for (float& component : key.value) {
            component *= inverseLength;
        }
    } else {
        key.value[3] = 1.0F;
    }
    return key;
}

PositionKey readQuantizedPosition(std::span<const std::uint8_t> data, std::size_t offset,
                                  const BoneSectionHeader& header) {
    PositionKey key;
    for (std::size_t component = 0; component < key.value.size(); ++component) {
        const std::int32_t step =
            (header.positionMaximum[component] - header.positionMinimum[component]) >> 8;
        key.value[component] = header.positionMinimum[component] +
            static_cast<std::int32_t>(data[offset + component]) * step;
    }
    return key;
}

} // namespace

bool decodePlivV23Header(std::span<const std::uint8_t> data, ActionDescriptor& action,
                         std::string& error) {
    action = ActionDescriptor{};
    if (data.size() < 26U || data[0] != 'P' || data[1] != 'L' || data[2] != 'M') {
        error = "not a PLIV payload";
        return false;
    }
    action.version = data[3];
    if (action.version != 23U) {
        error = "unsupported PLIV version";
        return false;
    }
    const std::uint8_t motionCount = data[4];
    action.auxiliaryObjectCount = data[5];
    if (motionCount == 0U) {
        error = "PLIV has no motions";
        return false;
    }
    const std::size_t tableBytes = static_cast<std::size_t>(motionCount) * 7U;
    if (26U + tableBytes > data.size()) {
        error = "truncated PLIV motion table";
        return false;
    }
    action.motions.reserve(motionCount);
    for (std::size_t index = 0; index < motionCount; ++index) {
        const std::size_t offset = 26U + index * 7U;
        MotionDescriptor motion;
        motion.featureFlags = readLe16(data, offset);
        motion.attachmentTrackCount = data[offset + 2U];
        motion.framesPerSecond = data[offset + 3U];
        motion.sampleCount = data[offset + 4U];
        motion.frameCount = readLe16(data, offset + 5U);
        if (motion.framesPerSecond == 0U || motion.frameCount == 0U) {
            error = "invalid PLIV motion timing";
            return false;
        }
        action.combinedFeatureFlags |= motion.featureFlags;
        action.motions.push_back(motion);
    }
    action.payloadOffset = 26U + tableBytes;
    return true;
}

bool decodePlivV23UnflowedTexture(std::span<const std::uint8_t> data,
                                 const ActionDescriptor& action, std::size_t offset,
                                 UnflowedTextureSection& section, std::string& error) {
    section = UnflowedTextureSection{};
    if (offset + 6U > data.size()) {
        error = "truncated unflowed-texture section header";
        return false;
    }
    section.declaredKeyCount = readLe16(data, offset + 4U);
    std::size_t cursor = offset + 6U;
    std::size_t decodedKeys = 0;
    for (std::size_t motionIndex = 0; motionIndex < action.motions.size(); ++motionIndex) {
        const auto& descriptor = action.motions[motionIndex];
        if ((descriptor.featureFlags & kPlivUnflowedTextureAnimation) == 0U) {
            continue;
        }
        if (cursor >= data.size()) {
            error = "truncated unflowed-texture motion";
            return false;
        }
        UnflowedTextureMotion motion;
        motion.motionIndex = motionIndex;
        const std::uint8_t trackCount = data[cursor++];
        motion.tracks.reserve(trackCount);
        for (std::size_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            if (cursor >= data.size()) {
                error = "truncated unflowed-texture track";
                return false;
            }
            UvTrack track;
            track.textureUnit = data[cursor++];
            const std::size_t maskBytes =
                static_cast<std::size_t>(descriptor.frameCount >> 3U) + 1U;
            if (cursor + maskBytes > data.size()) {
                error = "truncated unflowed-texture frame mask";
                return false;
            }
            const auto mask = data.subspan(cursor, maskBytes);
            cursor += maskBytes;
            track.frameToKey.resize(descriptor.frameCount);
            std::uint16_t currentKey = 0;
            for (std::size_t frame = 0; frame < descriptor.frameCount; ++frame) {
                const bool hasKey =
                    (mask[frame >> 3U] & (1U << (frame & 7U))) != 0U;
                if (hasKey) {
                    if (cursor + 4U > data.size()) {
                        error = "truncated unflowed-texture UV key";
                        return false;
                    }
                    currentKey = static_cast<std::uint16_t>(track.keys.size());
                    track.keys.push_back(UvKey{readLe16(data, cursor),
                                               readLe16(data, cursor + 2U)});
                    cursor += 4U;
                    ++decodedKeys;
                }
                track.frameToKey[frame] = currentKey;
            }
            motion.tracks.push_back(std::move(track));
        }
        section.motions.push_back(std::move(motion));
    }
    if (decodedKeys != section.declaredKeyCount) {
        error = "unflowed-texture key count mismatch";
        return false;
    }
    if (data.size() - cursor == 4U && data[cursor] == '*' && data[cursor + 1U] == '*' &&
        data[cursor + 2U] == '*' && data[cursor + 3U] == '*') {
        cursor += 4U;
    }
    section.consumedBytes = cursor;
    return true;
}

bool decodePlivV23BoneHeader(std::span<const std::uint8_t> data, std::size_t offset,
                            BoneSectionHeader& section, std::string& error) {
    section = BoneSectionHeader{};
    constexpr std::size_t fixedHeaderBytes = 7U + 24U;
    if (offset + fixedHeaderBytes > data.size()) {
        error = "truncated PLIV bone section header";
        return false;
    }
    section.boneCount = data[offset];
    section.rawFrameTableCapacity = readLe16(data, offset + 1U);
    section.rotationCacheCapacity = readLe16(data, offset + 3U);
    section.positionCacheCapacity = readLe16(data, offset + 5U);
    if (section.boneCount == 0U) {
        error = "PLIV bone section has no bones";
        return false;
    }
    for (std::size_t component = 0; component < 3U; ++component) {
        section.positionMinimum[component] =
            readLe32Signed(data, offset + 7U + component * 4U);
        section.positionMaximum[component] =
            readLe32Signed(data, offset + 19U + component * 4U);
    }
    std::size_t cursor = offset + fixedHeaderBytes;
    // Version 23 predates the extra leading hierarchy byte introduced in
    // version 24.  __pactLoadAniBone reads four hierarchy bytes, a 16-bit
    // vertex count, an 8-byte quaternion and three quantized position bytes.
    // Version 24+ reads the later five-byte hierarchy record instead.
    const std::size_t boneDescriptorBytes = data[3] <= 23U ? 17U : 18U;
    section.descriptorBytes = boneDescriptorBytes;
    if (cursor + static_cast<std::size_t>(section.boneCount) * boneDescriptorBytes >
        data.size()) {
        error = "truncated PLIV bone descriptor table";
        return false;
    }
    section.bones.reserve(section.boneCount);
    for (std::size_t index = 0; index < section.boneCount; ++index) {
        BoneDescriptor bone;
        const std::size_t identifierCount = data[3] <= 23U ? 4U : 5U;
        bone.identifiers.fill(0xFFU);
        for (std::size_t field = 0; field < identifierCount; ++field) {
            bone.identifiers[field] = data[cursor + field];
        }
        const std::size_t countOffset = identifierCount;
        bone.rawField = readLe16(data, cursor + countOffset);
        const std::size_t rotationOffset = countOffset + 2U;
        for (std::size_t component = 0; component < bone.bindRotation.size(); ++component) {
            bone.bindRotation[component] =
                readLe16Signed(data, cursor + rotationOffset + component * 2U);
        }
        const std::size_t positionOffset = rotationOffset + 8U;
        for (std::size_t component = 0; component < bone.bindPosition.size(); ++component) {
            bone.bindPosition[component] = data[cursor + positionOffset + component];
        }
        section.bones.push_back(bone);
        cursor += boneDescriptorBytes;
    }
    section.trackDataOffset = cursor;
    return true;
}

bool decodePlivV23Bone(std::span<const std::uint8_t> data, const ActionDescriptor& action,
                       std::size_t offset, BoneSection& section, std::string& error) {
    section = BoneSection{};
    if (!decodePlivV23BoneHeader(data, offset, section.header, error)) {
        return false;
    }
    std::size_t cursor = section.header.trackDataOffset;
    for (std::size_t motionIndex = 0; motionIndex < action.motions.size(); ++motionIndex) {
        const auto& descriptor = action.motions[motionIndex];
        if ((descriptor.featureFlags & kPlivBoneAnimation) == 0U) {
            continue;
        }
        BoneMotion motion;
        motion.motionIndex = motionIndex;
        motion.tracks.reserve(section.header.boneCount);
        for (std::size_t boneIndex = 0; boneIndex < section.header.boneCount; ++boneIndex) {
            const std::size_t maskBytes =
                static_cast<std::size_t>(descriptor.frameCount >> 3U) + 1U;
            const auto failTrack = [&](const char* message) {
                error = std::string(message) + " at bone " +
                    std::to_string(boneIndex) + ", motion " +
                    std::to_string(motionIndex) + ", offset " +
                    std::to_string(cursor);
                return false;
            };
            BoneTrack track;
            track.frameToRotationKey.resize(descriptor.frameCount);
            track.frameToPositionKey.resize(descriptor.frameCount);

            if (cursor >= data.size()) {
                return failTrack("truncated PLIV bone rotation track");
            }
            if (data[cursor] == 0U) {
                if (cursor + 9U > data.size()) {
                    return failTrack("truncated PLIV constant bone rotation");
                }
                track.rotationKeys.push_back(readQuaternion(data, cursor + 1U));
                cursor += 9U;
            } else {
                if (cursor + maskBytes > data.size()) {
                    return failTrack("truncated PLIV bone rotation mask");
                }
                const auto mask = data.subspan(cursor, maskBytes);
                cursor += maskBytes;
                std::uint16_t currentKey = 0;
                for (std::size_t frame = 0; frame < descriptor.frameCount; ++frame) {
                    if ((mask[frame >> 3U] & (1U << (frame & 7U))) != 0U) {
                        if (cursor + 8U > data.size() ||
                            track.rotationKeys.size() >= std::numeric_limits<std::uint16_t>::max()) {
                            return failTrack(
                                "truncated or oversized PLIV bone rotation keys");
                        }
                        currentKey = static_cast<std::uint16_t>(track.rotationKeys.size());
                        track.rotationKeys.push_back(readQuaternion(data, cursor));
                        cursor += 8U;
                    }
                    track.frameToRotationKey[frame] = currentKey;
                }
            }
            if (track.rotationKeys.size() == 1U) {
                std::fill(track.frameToRotationKey.begin(), track.frameToRotationKey.end(), 0U);
            }

            if (cursor >= data.size()) {
                return failTrack("truncated PLIV bone position track");
            }
            if (data[cursor] == 0U) {
                if (cursor + 4U > data.size()) {
                    return failTrack("truncated PLIV constant bone position");
                }
                track.positionKeys.push_back(
                    readQuantizedPosition(data, cursor + 1U, section.header));
                cursor += 4U;
            } else {
                if (cursor + maskBytes > data.size()) {
                    return failTrack("truncated PLIV bone position mask");
                }
                const auto mask = data.subspan(cursor, maskBytes);
                cursor += maskBytes;
                std::uint16_t currentKey = 0;
                for (std::size_t frame = 0; frame < descriptor.frameCount; ++frame) {
                    if ((mask[frame >> 3U] & (1U << (frame & 7U))) != 0U) {
                        if (cursor + 3U > data.size() ||
                            track.positionKeys.size() >= std::numeric_limits<std::uint16_t>::max()) {
                            return failTrack(
                                "truncated or oversized PLIV bone position keys");
                        }
                        currentKey = static_cast<std::uint16_t>(track.positionKeys.size());
                        track.positionKeys.push_back(
                            readQuantizedPosition(data, cursor, section.header));
                        cursor += 3U;
                    }
                    track.frameToPositionKey[frame] = currentKey;
                }
            }
            if (track.positionKeys.size() == 1U) {
                std::fill(track.frameToPositionKey.begin(), track.frameToPositionKey.end(), 0U);
            }
            section.storedRotationKeyCount += track.rotationKeys.size();
            section.storedPositionKeyCount += track.positionKeys.size();
            motion.tracks.push_back(std::move(track));
        }
        section.motions.push_back(std::move(motion));
    }
    section.consumedBytes = cursor;
    return true;
}

bool decodePlivV23VertexPrefix(std::span<const std::uint8_t> data, std::size_t offset,
                              VertexSectionPrefix& section, std::string& error) {
    section = VertexSectionPrefix{};
    if (offset + 6U > data.size()) {
        error = "truncated PLIV vertex section prefix";
        return false;
    }
    std::size_t cursor = offset + 4U;
    const std::size_t remapCount = readLe16(data, cursor);
    cursor += 2U;
    if (cursor + remapCount * 2U > data.size()) {
        error = "truncated PLIV vertex remap table";
        return false;
    }
    section.remapIndices.reserve(remapCount);
    for (std::size_t index = 0; index < remapCount; ++index) {
        section.remapIndices.push_back(readLe16(data, cursor));
        cursor += 2U;
    }
    if (cursor + 2U > data.size()) {
        error = "truncated PLIV packed-entry count";
        return false;
    }
    const std::size_t packedEntryCount = readLe16(data, cursor);
    cursor += 2U;
    if (cursor + packedEntryCount * 4U > data.size()) {
        error = "truncated PLIV packed-entry table";
        return false;
    }
    section.packedEntries.reserve(packedEntryCount);
    for (std::size_t index = 0; index < packedEntryCount; ++index) {
        section.packedEntries.push_back(static_cast<std::uint32_t>(
            readLe32Signed(data, cursor)));
        cursor += 4U;
    }
    if (cursor + 2U > data.size()) {
        error = "truncated PLIV packed-triple count";
        return false;
    }
    const std::size_t packedTripleCount = readLe16(data, cursor);
    cursor += 2U;
    if (cursor + packedTripleCount * 3U > data.size()) {
        error = "truncated PLIV packed-triple table";
        return false;
    }
    section.packedTriples.reserve(packedTripleCount);
    for (std::size_t index = 0; index < packedTripleCount; ++index) {
        section.packedTriples.push_back(
            {data[cursor], data[cursor + 1U], data[cursor + 2U]});
        cursor += 3U;
    }
    if (cursor + 4U > data.size()) {
        error = "truncated PLIV vertex material-record header";
        return false;
    }
    section.reservedCount = readLe16(data, cursor);
    section.materialRecordCount = readLe16(data, cursor + 2U);
    cursor += 4U;
    section.vertexDataOffset = cursor;
    constexpr std::size_t vertexStrideV23 = 13U;
    if (cursor + static_cast<std::size_t>(section.materialRecordCount) *
            vertexStrideV23 >
        data.size()) {
        error = "truncated PLIV v23 vertex data";
        return false;
    }
    section.afterVertexDataOffset =
        cursor + static_cast<std::size_t>(section.materialRecordCount) *
            vertexStrideV23;
    return true;
}

bool decodePlivV23Vertex(std::span<const std::uint8_t> data,
                        const ActionDescriptor& action, std::size_t offset,
                        VertexAnimationSection& section, std::string& error) {
    section = VertexAnimationSection{};
    VertexSectionPrefix prefix;
    if (!decodePlivV23VertexPrefix(data, offset, prefix, error)) {
        return false;
    }

    section.remapIndices = prefix.remapIndices;
    section.texcoords.reserve(prefix.packedEntries.size());
    for (const std::uint32_t packed : prefix.packedEntries) {
        section.texcoords.push_back({
            static_cast<float>(packed & 0xffffU) / 256.0F,
            static_cast<float>(packed >> 16U) / 256.0F,
        });
    }

    std::size_t cursor = prefix.vertexDataOffset;
    section.materials.reserve(prefix.materialRecordCount);
    for (std::size_t recordIndex = 0;
         recordIndex < prefix.materialRecordCount; ++recordIndex) {
        VertexMaterialRecord material;
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(cursor),
                    material.raw.size(), material.raw.begin());
        cursor += material.raw.size();
        section.materials.push_back(material);
    }

    for (std::size_t motionIndex = 0; motionIndex < action.motions.size(); ++motionIndex) {
        const MotionDescriptor& descriptor = action.motions[motionIndex];
        if ((descriptor.featureFlags & kPlivVertexAnimation) == 0U) {
            continue;
        }
        constexpr std::size_t descriptorBytes = 37U;
        if (cursor + descriptorBytes > data.size()) {
            error = "truncated PLIV vertex motion descriptor";
            return false;
        }
        VertexAnimationMotion motion;
        motion.motionIndex = motionIndex;
        motion.remapOffset = readLe16(data, cursor);
        motion.remapCount = readLe16(data, cursor + 2U);
        motion.texcoordOffset = readLe16(data, cursor + 4U);
        motion.texcoordCount = readLe16(data, cursor + 6U);
        motion.packedPositionOffset = readLe16(data, cursor + 8U);
        motion.packedPositionCount = readLe16(data, cursor + 10U);
        motion.attachmentTrackCount = data[cursor + 12U];
        for (std::size_t component = 0; component < 3U; ++component) {
            motion.positionMinimum[component] =
                readLe32Signed(data, cursor + 13U + component * 4U);
            motion.positionMaximum[component] =
                readLe32Signed(data, cursor + 25U + component * 4U);
        }
        cursor += descriptorBytes;

        const std::size_t remapEnd =
            static_cast<std::size_t>(motion.remapOffset) + motion.remapCount;
        const std::size_t texcoordEnd =
            static_cast<std::size_t>(motion.texcoordOffset) + motion.texcoordCount;
        const std::size_t packedPositionEnd =
            static_cast<std::size_t>(motion.packedPositionOffset) +
            motion.packedPositionCount;
        if (remapEnd > prefix.remapIndices.size() ||
            texcoordEnd > prefix.packedEntries.size() ||
            packedPositionEnd > prefix.packedTriples.size() ||
            motion.texcoordCount == 0U || descriptor.frameCount == 0U ||
            motion.packedPositionCount !=
                static_cast<std::size_t>(motion.texcoordCount) * descriptor.frameCount) {
            error = "invalid PLIV vertex motion ranges";
            return false;
        }
        for (std::size_t remap = motion.remapOffset; remap < remapEnd; ++remap) {
            if (prefix.remapIndices[remap] >= motion.texcoordCount) {
                error = "PLIV vertex remap references an absent animated vertex";
                return false;
            }
        }

        motion.frames.reserve(descriptor.frameCount);
        for (std::size_t frameIndex = 0; frameIndex < descriptor.frameCount;
             ++frameIndex) {
            std::vector<Vec3> frame;
            frame.reserve(motion.texcoordCount);
            for (std::size_t vertexIndex = 0; vertexIndex < motion.texcoordCount;
                 ++vertexIndex) {
                const std::size_t packedIndex =
                    static_cast<std::size_t>(motion.packedPositionOffset) +
                    frameIndex * motion.texcoordCount + vertexIndex;
                const auto& packed = prefix.packedTriples[packedIndex];
                Vec3 position;
                float* components[] = {&position.x, &position.y, &position.z};
                for (std::size_t component = 0; component < 3U; ++component) {
                    const std::int64_t range =
                        static_cast<std::int64_t>(motion.positionMaximum[component]) -
                        motion.positionMinimum[component];
                    const std::int64_t fixed =
                        static_cast<std::int64_t>(motion.positionMinimum[component]) +
                        (range >> 8) * packed[component];
                    *components[component] = static_cast<float>(fixed) / 65536.0F;
                }
                frame.push_back(position);
            }
            motion.frames.push_back(std::move(frame));
        }
        section.motions.push_back(std::move(motion));
    }
    if (section.motions.empty()) {
        error = "PLIV has no vertex-animation motion";
        return false;
    }

    if (cursor + 2U > data.size()) {
        error = "truncated PLIV vertex frame-mask header";
        return false;
    }
    section.frameMaskByteCount = readLe16(data, cursor);
    cursor += 2U;
    if (cursor + section.frameMaskByteCount > data.size()) {
        error = "truncated PLIV vertex frame mask";
        return false;
    }
    section.frameMask.assign(data.begin() + static_cast<std::ptrdiff_t>(cursor),
                             data.begin() + static_cast<std::ptrdiff_t>(
                                 cursor + section.frameMaskByteCount));
    cursor += section.frameMaskByteCount;
    section.trailingData.assign(data.begin() + static_cast<std::ptrdiff_t>(cursor),
                                data.end());
    section.consumedBytes = data.size();
    error.clear();
    return true;
}

} // namespace pact
