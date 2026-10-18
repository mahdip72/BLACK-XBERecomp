// CPU-only reference for the optional radius <= 1 atlas-particle filter.
// The root agent owns compilation and execution; this file is not in CMake.
#include "d3d/particle_dof_shader.h"
#include "d3d/particle_dof_small_radius.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string_view>

namespace {

using Point = std::array<float, 2>;
using Texels = std::array<float, 9>;

std::array<Point, 64> ReadProductionDisk() {
    std::string_view text = gpu::kParticleDofDiskHlsl;
    const auto begin = text.find("static const float2 particleDisk");
    if (begin == text.npos) std::abort();
    text.remove_prefix(begin);
    std::array<Point, 64> disk{};
    for (auto& point : disk) {
        const auto at = text.find("float2(");
        if (at == text.npos) std::abort();
        text.remove_prefix(at + 7);
        char* end = nullptr;
        point[0] = std::strtof(text.data(), &end);
        if (end == text.data()) std::abort();
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        const auto comma = text.find(',');
        if (comma == text.npos) std::abort();
        text.remove_prefix(comma + 1);
        point[1] = std::strtof(text.data(), &end);
        if (end == text.data()) std::abort();
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
    }
    if (text.find("float2(") < text.find("};")) std::abort();
    return disk;
}

void Require(bool okay, const char* why) {
    if (!okay) {
        std::fprintf(stderr, "small-radius CPU reference failed: %s\n", why);
        std::exit(1);
    }
}

double Sample(const Texels& texels, double x, double y, bool quantize) {
    const int left = static_cast<int>(std::floor(x));
    const int top = static_cast<int>(std::floor(y));
    Require(left >= -1 && left <= 0 && top >= -1 && top <= 0, "3x3 support");
    double fx = x - left, fy = y - top;
    // A conservative model of eight fractional bits in hardware bilinear filtering.
    // This is a tolerance check, not a claim that every adapter rounds identically.
    if (quantize) {
        fx = std::round(fx * 256.0) / 256.0;
        fy = std::round(fy * 256.0) / 256.0;
    }
    auto at = [&](int tx, int ty) { return double(texels[(ty + 1) * 3 + tx + 1]); };
    return (1 - fy) * ((1 - fx) * at(left, top) + fx * at(left + 1, top)) +
        fy * ((1 - fx) * at(left, top + 1) + fx * at(left + 1, top + 1));
}

double Reference(const std::array<Point, 64>& disk, const Texels& texels,
                 double radius, bool quantize) {
    double result = 0;
    for (const auto& point : disk)
        result += Sample(texels, double(point[0]) * radius, double(point[1]) * radius, quantize);
    return result / disk.size();
}

double Candidate(const gpu::ParticleDofSmallRadiusWeights& weights, const Texels& texels) {
    double result = 0;
    for (std::size_t i = 0; i < texels.size(); ++i)
        result += double(weights.texel[i]) * texels[i];
    return result;
}

void CheckGuard(const std::array<Point, 64>& disk) {
    // The current shader writes content from low+2 through high-3, and retains
    // two transparent guard pixels on each side. Every 3x3 load stays in cell.
    constexpr int low = 11, high = 29;
    for (int pixel : {low + 2, high - 3})
        Require(pixel - 1 >= low && pixel + 1 < high, "cell guard texel bounds");
    for (const auto& point : disk) {
        Require(std::abs(point[0]) < 1 && std::abs(point[1]) < 1, "disk tap support");
        for (int pixel : {low + 2, high - 3}) {
            const double x = pixel + .5 + point[0];
            const double y = pixel + .5 + point[1];
            Require(x >= low + .5 && x <= high - .5 &&
                    y >= low + .5 && y <= high - .5, "cell guard sample bounds");
        }
    }
}

} // namespace

int main() {
    const auto disk = ReadProductionDisk();
    CheckGuard(disk);
    gpu::ParticleDofSmallRadiusWeights weights{};
    weights.texel.fill(-7.0f);
    for (float bad : {-1.0f, 1.00001f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()}) {
        Require(!gpu::BuildParticleDofSmallRadiusWeights(bad, weights), "invalid radius rejected");
        for (float value : weights.texel) Require(value == -7.0f, "invalid radius leaves output intact");
    }

    std::mt19937 random(0x502DC0Fu);
    std::uniform_real_distribution<float> color(0.0f, 1.0f);
    std::uniform_real_distribution<float> radius_distribution(0.0f, 1.0f);
    double worst_ideal = 0, worst_quantized = 0;
    for (int trial = 0; trial < 2048; ++trial) {
        const float radius = trial < 7 ?
            std::array<float, 7>{0.0f, .001f, .05f, .125f, .5f, .999f, 1.0f}[trial] :
            radius_distribution(random);
        Require(gpu::BuildParticleDofSmallRadiusWeights(radius, weights), "valid radius accepted");
        double sum = 0;
        for (float value : weights.texel) {
            Require(value >= -1e-6f, "nonnegative weight");
            sum += value;
        }
        Require(std::abs(sum - 1.0) < 1e-6, "normalized weight sum");
        for (int image = 0; image < 4; ++image) {
            Texels texels{};
            for (float& value : texels) value = color(random);
            const double candidate = Candidate(weights, texels);
            worst_ideal = std::max(worst_ideal,
                std::abs(candidate - Reference(disk, texels, radius, false)));
            worst_quantized = std::max(worst_quantized,
                std::abs(candidate - Reference(disk, texels, radius, true)));
        }
    }
    Require(worst_ideal < 1e-6, "ideal 64-tap bilinear parity");
    Require(worst_quantized < .008, "eight-bit bilinear tolerance envelope");
    std::printf("small-radius CPU reference: ideal %.9g, quantized %.9g\n",
                worst_ideal, worst_quantized);
}
