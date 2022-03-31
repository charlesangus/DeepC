#pragma once

#include <vector>
#include <set>

#include "CircleMetadataKernel.hpp"

struct DOFKernel
{
    DOFKernel(const int blurRadius, const CircleMetadataKernel& metadataKernel);
    virtual ~DOFKernel();

    struct OffsetData
    {
        const int xOffset;
        float intensityCount;
        bool isEdgePixel;

        OffsetData(const int x, bool isEdgePixel) :
            xOffset(x), 
            intensityCount(1),
            isEdgePixel(isEdgePixel)
        {
        }

        OffsetData(const int x) : 
            xOffset(x),     
            intensityCount(0.0f),
            isEdgePixel(false)
        {
        }

        bool operator<(const OffsetData& other)
        {
            return xOffset < other.xOffset;
        }

        bool operator==(const OffsetData& other)
        {
            return xOffset == other.xOffset;
        }
    };

    std::vector<std::set<OffsetData>> offsets;
    std::size_t pixelCount;

    int blurRadius;
    std::size_t dimension;
};