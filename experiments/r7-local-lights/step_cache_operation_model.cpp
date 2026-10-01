#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <random>

struct Layer
{
    float start;
    float limit;
    float density;
};

int main()
{
    constexpr std::array<Layer, 4> layers = {{{0, 99999, 0.000393f}, {3000, 99999, 0.002f},
                                             {0, 99999, 0.002073f}, {122, 489, 0}}};
    constexpr int rays = 16384;
    std::mt19937 random(20261001);
    std::uniform_real_distribution<float> jitter(0.0f, 1.0f);
    std::cout << "{\n  \"scope\": \"base-layer density-noise fetch operation model; not GPU timing\",\n"
                 "  \"gpu_speedup_validated\": false,\n  \"cases\": [\n";
    int cases = 0;
    for (const int steps : {16, 24, 32})
    {
        for (const float distance : {100.0f, 500.0f, 2112.0f, 5000.0f})
        {
            std::uint64_t baseline = 0;
            std::uint64_t cached = 0;
            std::uint64_t boundarySamples = 0;
            for (int ray = 0; ray < rays; ++ray)
            {
                for (int step = 0; step < steps; ++step)
                {
                    const float stepFraction = 1.0f / steps;
                    const float startFraction = step * stepFraction;
                    const float endFraction = startFraction + stepFraction;
                    const float stepStart = distance * startFraction * startFraction;
                    const float stepEnd = distance * endFraction * endFraction;
                    const float mix = jitter(random);
                    const float sample = stepStart + (stepEnd - stepStart) * mix;
                    baseline += 2;
                    cached += 2;
                    for (const Layer& layer : layers)
                    {
                        const float start = std::max(stepStart, layer.start);
                        const float length = std::max(std::min(stepEnd, layer.limit) - start, 0.0f);
                        if (length <= 0 || layer.density <= 0)
                            continue;
                        baseline += 2;
                        if (start + length * mix != sample)
                        {
                            cached += 2;
                            ++boundarySamples;
                        }
                    }
                }
            }
            if (cases++)
                std::cout << ",\n";
            std::cout << "    {\"steps\": " << steps << ", \"ray_yards\": " << distance
                      << ", \"rays\": " << rays << ", \"baseline_fetches\": " << baseline
                      << ", \"cached_fetches\": " << cached << ", \"boundary_resamples\": " << boundarySamples
                      << ", \"fetch_reduction_percent\": " << 100.0 * (1.0 - static_cast<double>(cached) / baseline)
                      << "}";
        }
    }
    std::cout << "\n  ]\n}\n";
}
