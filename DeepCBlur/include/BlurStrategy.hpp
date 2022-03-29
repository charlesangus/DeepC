#pragma once

#include "DDImage/DeepOp.h"

#include "DeepCBlurSpec.hpp"

template<bool constrainBlur, bool volumetricBlur>
class BlurStrategy
{
protected:
    const DeepCBlurSpec& _deepCSpec;

public:
    BlurStrategy(const DeepCBlurSpec& blurSpec);
    virtual ~BlurStrategy();

    virtual void xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) = 0;
    virtual void yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) = 0;
    virtual void zBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel);
};

template<bool constrainBlur, bool volumetricBlur>
class RawGaussianStrategy : public BlurStrategy<constrainBlur, volumetricBlur>
{
public:
    RawGaussianStrategy(const DeepCBlurSpec& blurSpec);
    ~RawGaussianStrategy();

    void xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
    void yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
};

template<bool constrainBlur, bool volumetricBlur>
class TransparentModifiedGaussianStrategy : public BlurStrategy<constrainBlur, volumetricBlur>
{
public:
    TransparentModifiedGaussianStrategy(const DeepCBlurSpec& blurSpec);
    ~TransparentModifiedGaussianStrategy();

    void xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
    void yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
    void zBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
};

template<bool constrainBlur, bool volumetricBlur>
class ModifiedGaussianStrategy : public BlurStrategy<constrainBlur, volumetricBlur>
{
public:
    ModifiedGaussianStrategy(const DeepCBlurSpec& blurSpec);
    ~ModifiedGaussianStrategy();

    void xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
    void yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
};

#include "..\src\BlurStrategy.cpp"
