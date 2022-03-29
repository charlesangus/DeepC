
using namespace DD::Image;

static const float APPROX_ZERO = 0.00000000001f;

BlurStrategy::BlurStrategy(const DeepCBlurSpec& blurSpec) : _deepCSpec(blurSpec) 
{
}

BlurStrategy::~BlurStrategy()
{
}

template<bool constrainBlur>
RawGaussianStrategy<constrainBlur>::RawGaussianStrategy(const DeepCBlurSpec& blurSpec) : BlurStrategy(blurSpec)
{
}

template<bool constrainBlur>
RawGaussianStrategy<constrainBlur>::~RawGaussianStrategy()
{
}

template<bool constrainBlur>
void RawGaussianStrategy<constrainBlur>::xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel)
{
    const std::size_t currentSampleCount = currentPixel.getSampleCount();
    for (std::size_t isample = 0; isample < currentSampleCount; ++isample)
    {
        if (constrainBlur)
        {
            const float deepFront = currentPixel.getUnorderedSample(isample, Chan_DeepFront);
            //if this sample is not being blurred, then it has no effect on the target pixel
            if ((deepFront < _deepCSpec.nearZ) || (deepFront > _deepCSpec.farZ))
            {
                continue;
            }
        }
        foreach(z, channels)
        {
            if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue) || (z == Chan_Alpha))
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z) * _deepCSpec.kernel[pixelDistance]);
            }
            else
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
            }
        }
    }
}

template<bool constrainBlur>
void RawGaussianStrategy<constrainBlur>::yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel)
{
    const std::size_t currentSampleCount = currentPixel.getSampleCount();
    for (std::size_t isample = 0; isample < currentSampleCount; ++isample)
    {
        if (constrainBlur)
        {
            const float deepFront = currentPixel.getUnorderedSample(isample, Chan_DeepFront);
            //if this sample is not being blurred, then it has no effect on the target pixel
            if ((deepFront < _deepCSpec.nearZ) || (deepFront > _deepCSpec.farZ))
            {
                continue;
            }
        }
        foreach(z, channels)
        {
            if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue) || (z == Chan_Alpha))
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z) * _deepCSpec.kernel[pixelDistance]);
            }
            else
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
            }
        }
    }
}

template<bool constrainBlur>
void RawGaussianStrategy<constrainBlur>::zBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel)
{
    const std::size_t currentSampleCount = currentPixel.getSampleCount();
    for (std::size_t isample = 0; isample < currentSampleCount; ++isample)
    {
        if (constrainBlur)
        {
            const float deepFront = currentPixel.getUnorderedSample(isample, Chan_DeepFront);
            //if this sample is not being blurred, then it should not be modified
            if ((deepFront < _deepCSpec.nearZ) || (deepFront > _deepCSpec.farZ))
            {
                foreach(z, channels)
                {
                    outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
                }
            }
        }
        else
        {
            for (int ivolumetric = -_deepCSpec.zKernelRadius; ivolumetric <= _deepCSpec.zKernelRadius; ++ivolumetric)
            {
                foreach(z, channels)
                {
                    if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue) || (z == Chan_Alpha))
                    {
                        outPixel.push_back(currentPixel.getUnorderedSample(isample, z) * _deepCSpec.zKernel[abs(ivolumetric)]);
                    }
                    else if (z == Chan_DeepFront)
                    {
                        outPixel.push_back(currentPixel.getUnorderedSample(isample, z) + _deepCSpec.volumetricOffsets[ivolumetric + _deepCSpec.zKernelRadius].first);
                    }
                    else if (z == Chan_DeepBack)
                    {
                        outPixel.push_back(currentPixel.getUnorderedSample(isample, z) + _deepCSpec.volumetricOffsets[ivolumetric + _deepCSpec.zKernelRadius].second);
                    }
                    else
                    {
                        outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
                    }
                }
            }
        }
    }
}

template<bool constrainBlur>
TransparentModifiedGaussianStrategy<constrainBlur>::TransparentModifiedGaussianStrategy(const DeepCBlurSpec& blurSpec) : BlurStrategy(blurSpec)
{
}

template<bool constrainBlur>
TransparentModifiedGaussianStrategy<constrainBlur>::~TransparentModifiedGaussianStrategy()
{
}

template<bool constrainBlur>
void TransparentModifiedGaussianStrategy<constrainBlur>::xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel)
{
    const std::size_t currentSampleCount = currentPixel.getSampleCount();
    float cumulativeTransparency = 1.0f;
    for (std::size_t isample = 0; isample < currentSampleCount; ++isample)
    {
        if (constrainBlur)
        {
            const float deepFront = currentPixel.getUnorderedSample(isample, Chan_DeepFront);
            //if this sample is not being blurred, then it has no effect on the target pixel
            if ((deepFront < _deepCSpec.nearZ) || (deepFront > _deepCSpec.farZ))
            {
                cumulativeTransparency *= (1.0f - currentPixel.getUnorderedSample(isample, Chan_Alpha));
                continue;
            }
        }
        const float modificationValue = cumulativeTransparency * _deepCSpec.kernel[pixelDistance];
        foreach(z, channels)
        {
            if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue))
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z) * modificationValue);
            }
            else if (z == Chan_Alpha)
            {
                outPixel.push_back(APPROX_ZERO);
            }
            else
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
            }
        }
        cumulativeTransparency *= (1.0f - currentPixel.getUnorderedSample(isample, Chan_Alpha));
    }
}

template<bool constrainBlur>
void TransparentModifiedGaussianStrategy<constrainBlur>::yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel)
{
    const std::size_t currentSampleCount = currentPixel.getSampleCount();
    for (std::size_t isample = 0; isample < currentSampleCount; ++isample)
    {
        if (constrainBlur)
        {
            const float deepFront = currentPixel.getUnorderedSample(isample, Chan_DeepFront);
            //if this sample is not being blurred, then it has no effect on the target pixel
            if ((deepFront < _deepCSpec.nearZ) || (deepFront > _deepCSpec.farZ))
            {
                continue;
            }
        }
        foreach(z, channels)
        {
            if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue))
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z) * _deepCSpec.kernel[pixelDistance]);
            }
            else if (z == Chan_Alpha)
            {
                outPixel.push_back(APPROX_ZERO);
            }
            else
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
            }
        }
    }
}

template<bool constrainBlur>
void TransparentModifiedGaussianStrategy<constrainBlur>::zBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel)
{
    const std::size_t currentSampleCount = currentPixel.getSampleCount();
    for (std::size_t isample = 0; isample < currentSampleCount; ++isample)
    {
        if (constrainBlur)
        {
            const float deepFront = currentPixel.getUnorderedSample(isample, Chan_DeepFront);
            //if this sample is not being blurred, then it should not be modified
            if ((deepFront < _deepCSpec.nearZ) || (deepFront > _deepCSpec.farZ))
            {
                foreach(z, channels)
                {
                    outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
                }
            }
        }
        else
        {
            for (int ivolumetric = -_deepCSpec.zKernelRadius; ivolumetric <= _deepCSpec.zKernelRadius; ++ivolumetric)
            {
                foreach(z, channels)
                {
                    if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue))
                    {
                        outPixel.push_back(currentPixel.getUnorderedSample(isample, z) * _deepCSpec.zKernel[abs(ivolumetric)]);
                    }
                    else if (z == Chan_Alpha)
                    {
                        outPixel.push_back(APPROX_ZERO);
                    }
                    else if (z == Chan_DeepFront)
                    {
                        outPixel.push_back(currentPixel.getUnorderedSample(isample, z) + _deepCSpec.volumetricOffsets[ivolumetric + _deepCSpec.zKernelRadius].first);
                    }
                    else if (z == Chan_DeepBack)
                    {
                        outPixel.push_back(currentPixel.getUnorderedSample(isample, z) + _deepCSpec.volumetricOffsets[ivolumetric + _deepCSpec.zKernelRadius].second);
                    }
                    else
                    {
                        outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
                    }
                }
            }
        }
    }
}

template<bool constrainBlur>
ModifiedGaussianStrategy<constrainBlur>::ModifiedGaussianStrategy(const DeepCBlurSpec& blurSpec) : BlurStrategy(blurSpec)
{
}

template<bool constrainBlur>
ModifiedGaussianStrategy<constrainBlur>::~ModifiedGaussianStrategy()
{
}

template<bool constrainBlur>
void ModifiedGaussianStrategy<constrainBlur>::xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel)
{
    const std::size_t currentSampleCount = currentPixel.getSampleCount();
    float cumulativeTransparency = 1.0f;
    for (std::size_t isample = currentSampleCount; isample-- > 0;)
    {
        if (constrainBlur)
        {
            const float deepFront = currentPixel.getUnorderedSample(isample, Chan_DeepFront);
            //if this sample is not being blurred, then it has no effect on the target pixel
            if ((deepFront < _deepCSpec.nearZ) || (deepFront > _deepCSpec.farZ))
            {
                cumulativeTransparency *= (1.0f - currentPixel.getUnorderedSample(isample, Chan_Alpha));
                continue;
            }
        }
        const float modificationValue = cumulativeTransparency * _deepCSpec.kernel[pixelDistance];
        foreach(z, channels)
        {
            if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue) || (z == Chan_Alpha))
            {
                outPixel.push_back(currentPixel.getOrderedSample(isample, z) * modificationValue);
            }
            else
            {
                outPixel.push_back(currentPixel.getOrderedSample(isample, z));
            }
        }

        //this in a sense darkens the colour of the sample 
        //where the amount of darkening is dependendent on the samples in front of this sample (in the same pixel of the input image)
        //this mirrors the behaviour that a deep image must be compositied with the 'over operation' before a flat blur can occur
        //therefore in a flat blur the individual samples will have these contributions to the final image
        //compound the darkening effect for samples that have even more samples in front of them
        //decreases the sample alpha to balance out the decrease in RGB so that the un-premultiplied colour remains the same
        //if 'alpha <= 1.0', then '1 - alpha 'will always be a positive float, so darkening can happen indefinitely
        //if 'alpha > 1.0', then '1 - alpha' will be negative and will result in negative RGBAs for samples, which I consider undefined behaviour for this program
        cumulativeTransparency *= (1.0f - currentPixel.getOrderedSample(isample, Chan_Alpha));
    }
}

template<bool constrainBlur>
void ModifiedGaussianStrategy<constrainBlur>::yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel)
{
    const std::size_t currentSampleCount = currentPixel.getSampleCount();
    for (std::size_t isample = 0; isample < currentSampleCount; ++isample)
    {
        if (constrainBlur)
        {
            const float deepFront = currentPixel.getUnorderedSample(isample, Chan_DeepFront);
            if ((deepFront < _deepCSpec.nearZ) || (deepFront > _deepCSpec.farZ))
            {
                continue;
            }
        }
        foreach(z, channels)
        {
            if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue) || (z == Chan_Alpha))
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z) * _deepCSpec.kernel[pixelDistance]);
            }
            else
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
            }
        }
    }
}

template<bool constrainBlur>
void ModifiedGaussianStrategy<constrainBlur>::zBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel)
{
    const std::size_t currentSampleCount = currentPixel.getSampleCount();
    for (std::size_t isample = 0; isample < currentSampleCount; ++isample)
    {
        if (constrainBlur)
        {
            const float deepFront = currentPixel.getUnorderedSample(isample, Chan_DeepFront);
            //if this sample is not being blurred, then it should not be modified
            if ((deepFront < _deepCSpec.nearZ) || (deepFront > _deepCSpec.farZ))
            {
                foreach(z, channels)
                {
                    outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
                }
            }
        }
        else
        {
            for (int ivolumetric = -_deepCSpec.zKernelRadius; ivolumetric <= _deepCSpec.zKernelRadius; ++ivolumetric)
            {
                foreach(z, channels)
                {
                    if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue) || (z == Chan_Alpha))
                    {
                        outPixel.push_back(currentPixel.getUnorderedSample(isample, z) * _deepCSpec.zKernel[abs(ivolumetric)]);
                    }
                    else if (z == Chan_DeepFront)
                    {
                        outPixel.push_back(currentPixel.getUnorderedSample(isample, z) + _deepCSpec.volumetricOffsets[ivolumetric + _deepCSpec.zKernelRadius].first);
                    }
                    else if (z == Chan_DeepBack)
                    {
                        outPixel.push_back(currentPixel.getUnorderedSample(isample, z) + _deepCSpec.volumetricOffsets[ivolumetric + _deepCSpec.zKernelRadius].second);
                    }
                    else
                    {
                        outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
                    }
                }
            }
        }
    }
}