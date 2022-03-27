#pragma once

#include <vector>

#define _USE_MATH_DEFINES
#include "DDImage/DDMath.h"

#include "DeepCDebugging.hpp"

float getSD(const float blurRadius);

std::vector<float> getLQGaussianKernel(const float sd, const int blurRadius);

std::vector<float> getMQGaussianKernel(const float sd, const int blurRadius);

std::vector<float> getHQGaussianKernel(const float sd, const int blurRadius);