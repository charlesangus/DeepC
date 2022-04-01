#pragma once

#include <vector>
#include <unordered_map>
#include <unordered_set>

#define _USE_MATH_DEFINES
#include "DDImage/DDMath.h"

#include "DeepCDefocusSpec.hpp"

float getEdgeIntensity(const float blurRadius, const CircleMetadataKernel& metadataKernel);

float getInnerIntensity(const float blurRadius, const CircleMetadataKernel& metadataKernel);

CircleMetadataKernel getCircleMetadataKernel(int blurRadius);

DOFKernel getCircleKernel(const int blurRadius, const float, const CircleMetadataKernel& metadataKernel);

DOFKernel getSpiralKernel(const int blurRadius, const float distribution_percentage, const CircleMetadataKernel& metadataKernel);

#if 0
std::vector<float> getRawCircleKernel(const int blurRadius, const float, const CircleMetadataKernel& metadataKernel);

std::vector<float> getSpiralKernel(const int blurRadius, const float distribution_percentage, const CircleMetadataKernel& metadataKernel)

std::vector<float> getSeperableCircleKernels(const int blurRadius, const float, const CircleMetadataKernel& metadataKernel);
#endif