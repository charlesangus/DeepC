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
    const float zStep = ((blurRadius * 2.0f + 1.0f) / static_cast<float>(volumetricsAmount)) * zScale;
    volumetricOffsets = std::vector<float>(2 * volumetricsAmount);
    for (int ivolumetric = 0; ivolumetric < volumetricsAmount; ++ivolumetric)
    {
        volumetricOffsets[2 * ivolumetric] = { zStart + static_cast<float>(ivolumetric) * zStep};
        volumetricOffsets[2 * ivolumetric + 1] = { zStart + static_cast<float>(ivolumetric + 1) * zStep };
        //NOTE: the below line pre comutes volumetrics where all DeepFront/DeepBack values are unique to prevent any issues where two samples occupy the exact z value
        //volumetricOffsets[2 * ivolumetric + 1] = { zStart + static_cast<float>(ivolumetric + 1) * (zStep - (zScale / 10.0f) * (ivolumetric + 1)) };
    }
}








