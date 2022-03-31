#pragma once

#include <vector>
#include <set>

#define _USE_MATH_DEFINES
#include "DDImage/DDMath.h"

#include "CMGDebugging.hpp"
#include "DOFKernel.hpp"

struct DOFIntensities
{
    float innerPixelIntensity;
    float edgePixelIntensity;
};

DOFIntensities getDOFIntensities(const float blurRadius, const CircleMetadataKernel& metadataKernel);

CircleMetadataKernel getCircleMetadataKernel(int blurRadius);

std::vector<float> getRawCircleKernel(const int blurRadius, const float, const CircleMetadataKernel& metadataKernel);

std::vector<float> getSeperableCircleKernels(const int blurRadius, const float, const CircleMetadataKernel& metadataKernel);

DOFKernel getCircleKernel(const int blurRadius, const float, const CircleMetadataKernel& metadataKernel);

DOFKernel getSpiralKernel(const int blurRadius, const float distribution_size, const CircleMetadataKernel& metadataKernel);