#pragma once

#include <vector>
#include <functional>
#include <utility>

enum DEEPC_BLUR_TYPE_NAME
{
    DEEPC_GAUSSIAN_BLUR = 0,

    DEEPC_BLUR_TYPE_UNKNOWN
};

enum DEEPC_BLUR_QUALITY_NAME
{
    DEEPC_LOW_QUALITY = 0,
    DEEPC_MEDIUM_QUALITY,
    DEEPC_HIGH_QUALITY,

    DEEPC_BLUR_QUALITY_UNKNOWN
};

enum DEEPC_BLUR_MODE_NAME
{
    DEEPC_RAW_GAUSSIAN = 0,
    DEEPC_TRANSPARENT_RAW_GAUSSIAN_MODE,
    DEEPC_MODIFIED_GAUSSIAN_MODE,

    DEEPC_BLUR_MODE_UNKNOWN
};

enum DEEPC_DEFOCUS_TYPE_NAME
{
    DEEPC_CIRCLE,

    DEEPC_DEFOCUS_TYPE_UNKNOWN
};

enum DEEPC_DEFOCUS_QUALITY_NAME
{
    DEEPC_FULL_CIRCLE = 0,
    DEEPC_APPROXIMATED_CIRCLE,

    DEEPC_DEFOCUS_QUALITY_UNKNOWN
};

enum DEEPC_DEFOCUS_MODE_NAME
{
    DEEPC_DEFOCUS_MODE_UNKNOWN
};


struct DeepCSpec
{
    DeepCSpec();
    virtual ~DeepCSpec();

    int blurType;
    int blurQuality;
    int blurMode;

    bool doZBlur;
    float zScale;
    int zKernelRadius;
    std::vector<float> zKernel;

    int volumetricsAmount;
    std::vector<std::pair<float,float>> volumetricOffsets;

    void computeVolumetrics(const float blurRadius);
};