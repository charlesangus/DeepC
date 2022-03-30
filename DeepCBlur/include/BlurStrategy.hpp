#pragma once

#include "DDImage/DeepOp.h"

#include "DeepCBlurSpec.hpp"

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
class BlurStrategy
{
protected:
    const DeepCBlurSpec& _deepCSpec;

public:
    BlurStrategy(const DeepCBlurSpec& blurSpec);
    virtual ~BlurStrategy();

    inline void xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel);
    inline void yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel);
    inline void zBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel);

    virtual inline void pushXSample(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const float kernelValue, DD::Image::DeepOutPixel& outPixel, float& cumulativeTransparency) = 0;
    virtual inline void pushYSample(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const float kernelValue, DD::Image::DeepOutPixel& outPixel);
    virtual inline void pushZSamples(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const std::vector<float>& volumetricOffsets, DD::Image::DeepOutPixel& outPixel);
};

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
class RawGaussianStrategy : public BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>
{
public:
    RawGaussianStrategy(const DeepCBlurSpec& blurSpec);
    ~RawGaussianStrategy();

    inline void pushXSample(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const float kernelValue, DD::Image::DeepOutPixel& outPixel, float& cumulativeTransparency) override;
};

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
class TransparentModifiedGaussianStrategy : public BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>
{
public:
    TransparentModifiedGaussianStrategy(const DeepCBlurSpec& blurSpec);
    ~TransparentModifiedGaussianStrategy();

private:
    inline void pushXSample(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const float kernelValue, DD::Image::DeepOutPixel& outPixel, float& cumulativeTransparency) override;
    inline void pushYSample(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const float kernelValue, DD::Image::DeepOutPixel& outPixel) override;
    inline void pushZSamples(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const std::vector<float>& volumetricOffsets, DD::Image::DeepOutPixel& outPixel) override;
};

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
class ModifiedGaussianStrategy : public BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>
{
public:
    ModifiedGaussianStrategy(const DeepCBlurSpec& blurSpec);
    ~ModifiedGaussianStrategy();

private:
    inline void pushXSample(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const float kernelValue, DD::Image::DeepOutPixel& outPixel, float& cumulativeTransparency) override;
};

#include "..\src\BlurStrategy.cpp"
