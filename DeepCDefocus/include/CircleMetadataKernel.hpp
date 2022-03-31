#pragma once

#include <vector>

struct CircleMetadataKernel
{
    int blurRadius;
    std::size_t dimension;
    std::vector<int> distances;
    std::vector<std::size_t> pixelCounts;

    int getDistance(int xOff, int yOff) const;
    std::size_t getPixelCount(int blurRadius) const;
    std::size_t getEdgePixelAmount(int blurRadius) const;
    bool isEdgePixel(int xOff, int yOff, int blurRadius) const;
};