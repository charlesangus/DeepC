#include "DeepCBlurSpec.hpp"
#include "BlurKernels.hpp"

DeepCBlurSpec::DeepCBlurSpec()
{
    blurType = DEEPC_GAUSSIAN_BLUR;
    blurQuality = DEEPC_MEDIUM_QUALITY;
    blurMode = DEEPC_MODIFIED_GAUSSIAN_MODE;

    constrainBlur = false;
    nearZ = 1.0f;
    farZ = 2.0f;
    blurFalloff = false;
    nearFalloffRate = 1.0f;
    farFalloffRate = 1.0f;
}

DeepCBlurSpec::~DeepCBlurSpec()
{
}

bool DeepCBlurSpec::init(const float blurSize, const float nearFalloffRate_, const float farFalloffRate_, const float falloffScale)
{
    blurRadius = blurSize * 1.5f;
    blurRadiusFloor = static_cast<int>(blurRadius);

    auto kernelFunction = getKernelFunction();
    if (kernelFunction == nullptr)
    {
        return false;
    }

    kernel = std::move(kernelFunction(getSD(blurSize), blurRadiusFloor));
    zKernel = std::move(kernelFunction(getSD(blurSize), zKernelRadius));
    volumetricOffsets = std::move(computeVolumetrics(blurRadiusFloor));

    if (constrainBlur && blurFalloff)
    {
        falloffKernels = std::vector<std::vector<float>>(blurRadiusFloor - 1);
        volumetricFalloffOffsets = std::vector<std::vector<float>>(blurRadiusFloor - 1);
        for (int radius = 1; radius < blurRadiusFloor; ++radius)
        {
            falloffKernels[radius - 1] = std::move(kernelFunction(getSD(static_cast<float>(radius) * 1.5f), radius));
            volumetricFalloffOffsets[radius - 1] = std::move(computeVolumetrics(radius));
        }

        nearFalloffRate = nearFalloffRate_ * falloffScale;
        farFalloffRate = farFalloffRate_ * falloffScale;
    }

    return true;
}

std::function<std::vector<float>(const float sd, const int blurRadius)> DeepCBlurSpec::getKernelFunction() const
{
    switch (blurType)
    {
        case DEEPC_GAUSSIAN_BLUR:
            switch (blurQuality)
            {
                case DEEPC_LOW_QUALITY:    return getLQGaussianKernel;
                case DEEPC_MEDIUM_QUALITY: return getMQGaussianKernel;
                case DEEPC_HIGH_QUALITY:   return getHQGaussianKernel;
                default:                 return nullptr;
            }
        default:                         return nullptr;
    }
}