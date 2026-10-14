#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pact {

struct Vec2 { float x{}; float y{}; };
struct Vec3 { float x{}; float y{}; float z{}; };

struct Mesh {
    std::uint8_t version{};
    std::uint8_t flags{};
    std::uint8_t attributeCount{};
    std::array<Vec3, 2> bounds{};
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> texcoords;
    std::vector<std::uint16_t> indices;
    std::size_t consumedBytes{};
};

bool decodePmodV23(std::span<const std::uint8_t> data, Mesh& mesh, std::string& error);

} // namespace pact
