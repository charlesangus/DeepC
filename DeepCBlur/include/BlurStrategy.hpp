#pragma once

#include "DDImage/DeepOp.h"

#include "DeepCBlurSpec.hpp"

class BlurStrategy
{
protected:
	const DeepCBlurSpec& _deepCSpec;

public:
	BlurStrategy(const DeepCBlurSpec& blurSpec);
	virtual ~BlurStrategy();

	virtual void xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) = 0;
	virtual void yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) = 0;
	virtual void zBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) = 0;
};

class RawGaussianStrategy : public BlurStrategy
{
public:
	RawGaussianStrategy(const DeepCBlurSpec& blurSpec);
	~RawGaussianStrategy();

	void xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
	void yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
	void zBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
};

class TransparentModifiedGaussianStrategy : public BlurStrategy
{
public:
	TransparentModifiedGaussianStrategy(const DeepCBlurSpec& blurSpec);
	~TransparentModifiedGaussianStrategy();

	void xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
	void yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
	void zBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
};

class ModifiedGaussianStrategy : public BlurStrategy
{
public:
	ModifiedGaussianStrategy(const DeepCBlurSpec& blurSpec);
	~ModifiedGaussianStrategy();

	void xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
	void yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
	void zBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel) override;
};