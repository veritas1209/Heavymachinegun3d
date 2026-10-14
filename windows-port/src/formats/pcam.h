#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "formats/pmod.h"

namespace pact {

struct CameraTrack {
    std::uint8_t version{};
    std::uint8_t flags{};
    std::uint8_t cameraId{};
    std::uint8_t framesPerSecond{};
    std::uint16_t frameCount{};
    float fieldOfViewDegrees{};
    float rollDegrees{};
    std::vector<Vec3> positions;
    std::vector<Vec3> targets;
    std::size_t consumedBytes{};
};

bool decodePcamV23(std::span<const std::uint8_t> data, CameraTrack& camera,
                   std::string& error);

} // namespace pact
