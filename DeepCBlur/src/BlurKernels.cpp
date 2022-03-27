#include "BlurKernels.hpp"
#include "DeepCBlurSpec.hpp"

static const float SQRT_2_MULT_PI = sqrtf(2.0f * M_PI_F);

float getSD(const float blurSize)
{
    return blurSize * 0.42466f;
    //return blurRadius / 3.0f;
}

void printKernelInfo(const DEEPC_BLUR_QUALITY_NAME quality, const float sd, const int blurRadius, const std::vector<float>& kernel)
{
    std::string qualityStr;
    switch (quality)
    {
        case DEEPC_LOW_QUALITY:
            qualityStr = "low";
            break;
        case DEEPC_MEDIUM_QUALITY:
            qualityStr = "medium";
            break;
        case DEEPC_HIGH_QUALITY:
            qualityStr = "high";
            break;
        default:
            qualityStr = "unknown";
            break;
    }
    DEEPC_PRINT("guassian kernel quality: \'%s\', sd: \'%f\', radius: \'%d\'\n", qualityStr.c_str(), sd, blurRadius);

    const std::size_t kernelSize = kernel.size();

    DEEPC_PRINT("gaussian kernel: \'[ ");
    for (std::size_t i = 0; i < kernelSize - 1; ++i)
    {
        DEEPC_PRINT("%f , ", kernel[i]);
    }
    DEEPC_PRINT("%f ]'\n", kernel[kernelSize - 1]);

    float total = kernel[0];
    if (kernelSize == (static_cast<std::size_t>(blurRadius) + 1))
    {
        for (int i = 1; i < kernelSize; ++i)
        {
            //multiply by 2 to compensate for only storing half the kernel
            total += (2.0f * kernel[i]);
        }
    }
    else
    {
        for (int i = 1; i < kernelSize; ++i)
        {
            total += kernel[i];
        }
    }

    DEEPC_PRINT("                 sum = %f\n", total);
}

std::vector<float> getLQGaussianKernel(const float sd, const int blurRadius)
{
    //only storing half the kernel, as the kernel is symmetrical
    const std::size_t compressedKernelSize = 1 + blurRadius;
    std::vector<float> kernel(compressedKernelSize, 0.0f);

    const float constant1 = 2.0f * sd * sd;
    const float constant2 = sd * SQRT_2_MULT_PI;

    kernel[0] = 1.0f / constant2;

    for (int i = 1; i < compressedKernelSize; ++i)
    {
        kernel[i] = exp(-static_cast<float>(i * i) / constant1) / constant2;
    }

#ifdef DEEPC_DEBUG_PRINT
    printKernelInfo(DEEPC_LOW_QUALITY, sd, blurRadius, kernel);
#endif

    return kernel;
}

std::vector<float> getMQGaussianKernel(const float sd, const int blurRadius)
{
    //only storing half the kernel, as the kernel is symmetrical
    const std::size_t compressedKernelSize = 1 + blurRadius;
    std::vector<float> kernel(compressedKernelSize, 0.0f);

    const float constant1 = 2.0f * sd * sd;
    const float constant2 = sd * SQRT_2_MULT_PI;

    kernel[0] = 1 / constant2;
    float total = kernel[0];

    for (int i = 1; i < compressedKernelSize; ++i)
    {
        kernel[i] = exp(-static_cast<float>(i * i) / constant1) / constant2;
        //multiply by 2 to compensate for only storing half the kernel
        total += (2.0f * kernel[i]);
    }

    for (std::size_t i = 0; i < compressedKernelSize; ++i)
    {
        kernel[i] /= total;
    }

#ifdef DEEPC_DEBUG_PRINT
    printKernelInfo(DEEPC_MEDIUM_QUALITY, sd, blurRadius, kernel);
#endif

    return kernel;
}

std::vector<float> getHQGaussianKernel(const float sd, const int blurRadius)
{
    //only storing half the kernel, as the kernel is symmetrical
    const std::size_t compressedKernelSize = 1 + blurRadius;
    const std::size_t actualKernelSize = 1 + 2 * blurRadius;

    std::vector<float> kernel(compressedKernelSize, 0.0f);

    auto cdf = [](const float x, const float sd)
    {
        return 0.5f * (1.0f + erff(x / (sd * M_SQRT2_F)));
    };


    //kernel covers -3.5 sd to 3.5 sd of the distribution
    const float step = (7.0f * sd) / actualKernelSize;
    float x = -3.5f * sd;
    float totalArea = cdf(x, sd);
    x += step;

    for (std::size_t i = 0; i < compressedKernelSize; ++i)
    {
        const float value = cdf(x, sd) - totalArea;
        kernel[compressedKernelSize - 1 - i] = value;
        totalArea += value;
        x += step;
    }

    //normalise the kernel
    float total = kernel[0];
    for (int i = 1; i < compressedKernelSize; ++i)
    {
        //multiply by 2 to compensate for only storing half the kernel
        total += (2.0f * kernel[i]);
    }

    for (std::size_t i = 0; i < compressedKernelSize; ++i)
    {
        kernel[i] /= total;
    }

#ifdef DEEPC_DEBUG_PRINT
    printKernelInfo(DEEPC_HIGH_QUALITY, sd, blurRadius, kernel);
#endif

    return kernel;
}