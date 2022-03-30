#pragma once

#include "DeepCSpec.hpp"

struct DeepCBlurSpec : public DeepCSpec
{
    float blurRadius;
    int blurRadiusFloor;
    std::vector<float> kernel;

    bool constrainBlur;
    float nearZ;
    float farZ;
    bool blurFalloff;
    float nearFalloffRate;
    float farFalloffRate;
    float fallofScale;
    std::vector<std::vector<float>> falloffKernels;

    DeepCBlurSpec();
    ~DeepCBlurSpec();

    std::function<std::vector<float>(const float sd, const int blurRadius)> getKernelFunction() const;
};