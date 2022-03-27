#include "DeepCSpec.hpp"

DeepCSpec::DeepCSpec() : 
    doZBlur(false),
    zScale(1.0),
    zKernelRadius(3)
{
}

DeepCSpec::~DeepCSpec()
{
}

void DeepCSpec::computeVolumetrics(const float blurRadius)
{
    volumetricsAmount = 2 * zKernelRadius + 1;
    const float zStart = -(blurRadius + 0.5f) * zScale;
    const float zStep = (blurRadius * 2.0f + 1.0f) / static_cast<float>(volumetricsAmount);
    volumetricOffsets = std::vector<std::pair<float, float>>(volumetricsAmount);
    for (int ivolumetric = 0; ivolumetric < volumetricsAmount; ++ivolumetric)
    {
        volumetricOffsets[ivolumetric] = { zStart + (static_cast<float>(ivolumetric) * zStep), zStart + (static_cast<float>(ivolumetric + 1) * (zStep)) };
    }
}








