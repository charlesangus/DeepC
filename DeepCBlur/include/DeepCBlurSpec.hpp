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
    
    std::vector<std::vector<float>> falloffKernels;

    DeepCBlurSpec();
    ~DeepCBlurSpec();

    bool init(const float blurSize, const float nearFalloffRate_, const float farFalloffRate_, const float falloffScale);

    std::function<std::vector<float>(const float sd, const int blurRadius)> getKernelFunction() const;
};