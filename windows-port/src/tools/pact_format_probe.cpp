#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <span>
#include <string>
#include <vector>

#include "formats/a3d.h"
#include "formats/pcam.h"
#include "formats/peft.h"
#include "formats/pliv.h"
#include "formats/pmod.h"

namespace fs = std::filesystem;

namespace {

struct Header {
    std::string kind;
    std::uint8_t version{};
    std::uint8_t primaryCount{};
    std::uint8_t secondaryCount{};
    std::uint8_t flags{};
    std::uint16_t rawCountA{};
    std::uint16_t rawCountB{};
};

std::uint16_t readLe16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
        static_cast<std::uint16_t>(data[offset + 1] << 8U);
}

bool parseHeader(std::span<const std::uint8_t> data, Header& header) {
    if (data.size() < 6 || data[0] != 'P') {
        return false;
    }

    const std::array<char, 3> magic{
        static_cast<char>(data[0]), static_cast<char>(data[1]), static_cast<char>(data[2])};
    const std::string signature(magic.data(), magic.size());
    if (signature == "PMM") {
        header.kind = "PMOD/mesh";
        header.version = data[3];
        header.flags = data[4];
        header.secondaryCount = data[5];
        if (data.size() >= 10) {
            header.rawCountA = readLe16(data, 6);
            header.rawCountB = readLe16(data, 8);
        }
        return true;
    }
    if (signature == "PCA") {
        header.kind = "PCAM/camera";
        header.version = data[3];
        header.primaryCount = data[4];
        header.secondaryCount = data[5];
        return true;
    }
    if (signature == "PEM") {
        header.kind = "PEFT/effect";
        header.version = data[3];
        header.primaryCount = data[4];
        header.secondaryCount = data[5];
        return true;
    }
    if (signature == "PLM") {
        header.kind = "PLIV/action";
        header.version = data[3];
        header.primaryCount = data[4];
        header.secondaryCount = data[5];
        return true;
    }
    return false;
}

std::vector<std::uint8_t> readPrefix(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(64);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(stream.gcount()));
    return bytes;
}

std::vector<std::uint8_t> readAll(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto length = stream.tellg();
    if (length <= 0) {
        return {};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: pact_format_probe <asset-directory>\n";
        return 2;
    }

    const fs::path root(argv[1]);
    if (!fs::is_directory(root)) {
        std::cerr << "asset directory not found: " << root.string() << '\n';
        return 3;
    }

    std::map<std::string, std::size_t> counts;
    std::size_t matched = 0;
    std::size_t decodedPmod = 0;
    std::size_t decodedPcam = 0;
    std::size_t decodedPeft = 0;
    std::size_t decodedPliv = 0;
    std::size_t decodedUnflowedTexture = 0;
    std::size_t decodedBoneHeaders = 0;
    std::size_t decodedBoneTracks = 0;
    std::size_t decodedVertexPrefixes = 0;
    std::size_t decodedVertexAnimations = 0;
    std::size_t decodedA3d = 0;
    std::cout << "relative_path\tbytes\tkind\tversion\tprimary\tsecondary\tflags\traw_count_a\traw_count_b\tpayload\n";
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string lowerName = entry.path().filename().string();
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        if (lowerName.find(".a3d") != std::string::npos) {
            const auto complete = readAll(entry.path());
            pact::A3dTable table;
            std::string error;
            ++matched;
            ++counts["A3D/table"];
            std::string payload;
            if (pact::decodeA3d(complete, table, error)) {
                ++decodedA3d;
                std::size_t stringFields = 0;
                for (const auto& field : table.fields) {
                    if (field.type == 0U) {
                        ++stringFields;
                    }
                }
                payload = "decoded-rows=" + std::to_string(table.rowCount) +
                    "-fields=" + std::to_string(table.fields.size()) +
                    "-row-size=" + std::to_string(table.rowSize) +
                    "-string-fields=" + std::to_string(stringFields);
            } else {
                payload = "error-" + error;
            }
            std::cout << fs::relative(entry.path(), root).generic_string() << '\t'
                      << entry.file_size() << "\tA3D/table\t0\t"
                      << table.rowCount << '\t' << table.fields.size()
                      << "\t0\t" << table.headerSize << '\t' << table.rowSize
                      << '\t' << payload << '\n';
            continue;
        }
        const auto bytes = readPrefix(entry.path());
        Header header;
        if (!parseHeader(bytes, header)) {
            continue;
        }
        ++matched;
        ++counts[header.kind];
        std::string payload = "header-only";
        if (header.kind == "PMOD/mesh") {
            const auto complete = readAll(entry.path());
            pact::Mesh mesh;
            std::string error;
            if (pact::decodePmodV23(complete, mesh, error)) {
                ++decodedPmod;
                payload = "decoded-v=" + std::to_string(mesh.positions.size()) +
                    "-i=" + std::to_string(mesh.indices.size()) +
                    "-tail=" + std::to_string(complete.size() - mesh.consumedBytes);
            } else {
                payload = "error-" + error;
            }
        } else if (header.kind == "PCAM/camera") {
            const auto complete = readAll(entry.path());
            pact::CameraTrack camera;
            std::string error;
            if (pact::decodePcamV23(complete, camera, error)) {
                ++decodedPcam;
                payload = "decoded-frames=" + std::to_string(camera.frameCount) +
                    "-fps=" + std::to_string(camera.framesPerSecond) +
                    "-tail=" + std::to_string(complete.size() - camera.consumedBytes);
            } else {
                payload = "error-" + error;
            }
        } else if (header.kind == "PEFT/effect") {
            const auto complete = readAll(entry.path());
            pact::EffectDescriptor effect;
            std::string error;
            if (pact::decodePeftV23(complete, effect, error)) {
                ++decodedPeft;
                std::ostringstream flags;
                flags << std::hex << effect.combinedFeatureFlags;
                payload = "decoded-motions=" + std::to_string(effect.motions.size()) +
                    "-flags=0x" + flags.str() +
                    "-fps=" + std::to_string(effect.motions.front().framesPerSecond) +
                    "-frames=" + std::to_string(effect.motions.front().frameCount) +
                    "-vertex-remaps=" +
                    std::to_string(effect.vertexPrefix.remapIndices.size()) +
                    "-vertex-packed=" +
                    std::to_string(effect.vertexPrefix.packedEntries.size()) +
                    "-vertex-triples=" +
                    std::to_string(effect.vertexPrefix.packedTriples.size()) +
                    "-aux-bytes=" +
                    std::to_string(effect.vertexPrefix.auxiliaryBytes.size()) +
                    "-base-records=" +
                    std::to_string(effect.vertexPrefix.baseRecords.size()) +
                    "-primary-mask-bytes=" +
                    std::to_string(effect.vertexPrefix.primaryFrameMasks.size()) +
                    "-secondary-mask-bytes=" +
                    std::to_string(effect.vertexPrefix.secondaryFrameMasks.size()) +
                    "-tail=" +
                    std::to_string(complete.size() -
                        effect.vertexPrefix.consumedBytes);
            } else {
                payload = "error-" + error;
            }
        } else if (header.kind == "PLIV/action") {
            const auto complete = readAll(entry.path());
            pact::ActionDescriptor action;
            std::string error;
            if (pact::decodePlivV23Header(complete, action, error)) {
                ++decodedPliv;
                std::ostringstream flags;
                flags << std::hex << action.combinedFeatureFlags;
                payload = "decoded-motions=" + std::to_string(action.motions.size()) +
                    "-flags=0x" + flags.str() +
                    "-payload-offset=" + std::to_string(action.payloadOffset) +
                    "-payload-bytes=" + std::to_string(complete.size() - action.payloadOffset);
                std::size_t sectionOffset = action.payloadOffset;
                if ((action.combinedFeatureFlags & pact::kPlivBoneAnimation) != 0U) {
                    pact::BoneSection boneSection;
                    if (pact::decodePlivV23Bone(
                            complete, action, sectionOffset, boneSection, error)) {
                        ++decodedBoneHeaders;
                        ++decodedBoneTracks;
                        sectionOffset = boneSection.consumedBytes;
                        payload += "-bones=" + std::to_string(boneSection.header.boneCount) +
                            "-rotation-cache=" +
                            std::to_string(boneSection.header.rotationCacheCapacity) +
                            "-rotation-stored=" +
                            std::to_string(boneSection.storedRotationKeyCount) +
                            "-position-cache=" +
                            std::to_string(boneSection.header.positionCacheCapacity) +
                            "-position-stored=" +
                            std::to_string(boneSection.storedPositionKeyCount) +
                            "-bone-end=" + std::to_string(boneSection.consumedBytes);
                    } else {
                        payload = "error-" + error;
                    }
                }
                const bool canDecodeUnflowed =
                    (action.combinedFeatureFlags & pact::kPlivUnflowedTextureAnimation) != 0U &&
                    (action.combinedFeatureFlags & pact::kPlivFlowedTextureAnimation) == 0U &&
                    payload.rfind("error-", 0U) != 0U;
                if (canDecodeUnflowed) {
                    pact::UnflowedTextureSection section;
                    if (pact::decodePlivV23UnflowedTexture(
                            complete, action, sectionOffset, section, error)) {
                        ++decodedUnflowedTexture;
                        sectionOffset = section.consumedBytes;
                        payload += "-unflowed-keys=" +
                            std::to_string(section.declaredKeyCount) +
                            "-unflowed-tail=" +
                            std::to_string(complete.size() - section.consumedBytes);
                    } else {
                        payload += "-unflowed-error=" + error +
                            "-unflowed-start=" + std::to_string(sectionOffset);
                    }
                }
                if ((action.combinedFeatureFlags & pact::kPlivVertexAnimation) != 0U &&
                    payload.rfind("error-", 0U) != 0U) {
                    if ((action.combinedFeatureFlags & pact::kPlivBoneAnimation) != 0U) {
                        const std::array<std::uint8_t, 4> footer{{'*', '*', '*', '*'}};
                        const auto footerAt = std::search(
                            complete.begin() + static_cast<std::ptrdiff_t>(sectionOffset),
                            complete.end(), footer.begin(), footer.end());
                        if (footerAt != complete.end()) {
                            sectionOffset = static_cast<std::size_t>(
                                std::distance(complete.begin(), footerAt));
                            payload += "-combined-vertex-start=" +
                                std::to_string(sectionOffset);
                        }
                    }
                    pact::VertexSectionPrefix vertexSection;
                    if (pact::decodePlivV23VertexPrefix(
                            complete, sectionOffset, vertexSection, error)) {
                        ++decodedVertexPrefixes;
                        payload += "-vertex-remaps=" +
                            std::to_string(vertexSection.remapIndices.size()) +
                            "-vertex-packed=" +
                            std::to_string(vertexSection.packedEntries.size()) +
                            "-vertex-triples=" +
                            std::to_string(vertexSection.packedTriples.size()) +
                            "-vertex-records=" +
                            std::to_string(vertexSection.materialRecordCount) +
                            "-vertex-data-end=" +
                            std::to_string(vertexSection.afterVertexDataOffset);
                        pact::VertexAnimationSection vertexAnimation;
                        if (pact::decodePlivV23Vertex(
                                complete, action, sectionOffset,
                                vertexAnimation, error)) {
                            ++decodedVertexAnimations;
                            const auto& motion = vertexAnimation.motions.front();
                            payload += "-vertex-frames=" +
                                std::to_string(motion.frames.size()) +
                                "-vertex-consumed=" +
                                std::to_string(vertexAnimation.consumedBytes);
                        } else {
                            payload += "-vertex-full-error=" + error;
                        }
                    } else {
                        payload += "-vertex-error=" + error +
                            "-vertex-start=" + std::to_string(sectionOffset);
                    }
                }
            } else {
                payload = "error-" + error;
            }
        }
        std::cout << fs::relative(entry.path(), root).generic_string() << '\t'
                  << entry.file_size() << '\t' << header.kind << '\t'
                  << static_cast<unsigned>(header.version) << '\t'
                  << static_cast<unsigned>(header.primaryCount) << '\t'
                  << static_cast<unsigned>(header.secondaryCount) << '\t'
                  << static_cast<unsigned>(header.flags) << '\t'
                  << header.rawCountA << '\t' << header.rawCountB << '\t' << payload << '\n';
    }

    std::cerr << "matched=" << matched;
    for (const auto& [kind, count] : counts) {
        std::cerr << " " << kind << "=" << count;
    }
    std::cerr << " decodedPmod=" << decodedPmod
              << " decodedPcam=" << decodedPcam
              << " decodedPeft=" << decodedPeft
              << " decodedPliv=" << decodedPliv
              << " decodedUnflowedTexture=" << decodedUnflowedTexture
              << " decodedBoneHeaders=" << decodedBoneHeaders
              << " decodedBoneTracks=" << decodedBoneTracks
              << " decodedVertexPrefixes=" << decodedVertexPrefixes
              << " decodedVertexAnimations=" << decodedVertexAnimations
              << " decodedA3d=" << decodedA3d << '\n';
    return matched == 0 ? 4 : 0;
}
