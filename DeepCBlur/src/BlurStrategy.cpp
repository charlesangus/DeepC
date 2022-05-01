#include "BlurStrategy.hpp"

using namespace DD::Image;

static const float APPROX_ZERO = 0.00000000001f;

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>::BlurStrategy(const DeepCBlurSpec& blurSpec) : _deepCSpec(blurSpec)
{
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>::~BlurStrategy()
{
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
void BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>::xBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel)
{
    const std::size_t currentSampleCount = currentPixel.getSampleCount();
    float cumulativeTransparency = 1.0f;
    for (std::size_t isample = currentSampleCount; isample-- > 0;)
    {
        //if only samples in a certain range should be blurred
        if (constrainBlur)
        {
            const float deepFront = currentPixel.getOrderedSample(isample, Chan_DeepFront);
            if (deepFront < _deepCSpec.nearZ)
            {
                //if the sample might need blurring to smooth the blurring transition
                if (blurFalloff)
                {
                    const int blurRadius = MIN(_deepCSpec.blurRadiusFloor - 1, _deepCSpec.blurRadiusFloor - static_cast<int>((_deepCSpec.nearZ - deepFront) / _deepCSpec.nearFalloffRate));
                    //if the calculated blur radius is valid and the sample is still in it's blur radius
                    if ((blurRadius >= 1) && (pixelDistance < (blurRadius + 1)))
                    {
                        pushXSample(currentPixel, isample, channels, _deepCSpec.falloffKernels[blurRadius - 1][pixelDistance], outPixel, cumulativeTransparency);
                    }
                }
                //else the sample is not blurred
                cumulativeTransparency *= (1.0f - currentPixel.getOrderedSample(isample, Chan_Alpha));
                continue;
            }
            if (deepFront > _deepCSpec.farZ)
            {
                //if the sample might need blurring to smooth the blurring transition
                if (blurFalloff)
                {
                    const int blurRadius = MIN(_deepCSpec.blurRadiusFloor - 1, _deepCSpec.blurRadiusFloor - static_cast<int>((deepFront - _deepCSpec.farZ) / _deepCSpec.farFalloffRate));
                    if ((blurRadius >= 1) && (pixelDistance < (blurRadius + 1)))
                    {
                        pushXSample(currentPixel, isample, channels, _deepCSpec.falloffKernels[blurRadius - 1][pixelDistance], outPixel, cumulativeTransparency);
                    }
                }
                //else the sample is not blurred
                cumulativeTransparency *= (1.0f - currentPixel.getOrderedSample(isample, Chan_Alpha));
                continue;
            }
        }

        pushXSample(currentPixel, isample, channels, _deepCSpec.kernel[pixelDistance], outPixel, cumulativeTransparency);
    }
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
void BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>::yBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel)
{
    const std::size_t currentSampleCount = currentPixel.getSampleCount();
    for (std::size_t isample = 0; isample < currentSampleCount; ++isample)
    {
        //if only samples in a certain range should be blurred
        if (constrainBlur)
        {
            const float deepFront = currentPixel.getOrderedSample(isample, Chan_DeepFront);
            if (deepFront < _deepCSpec.nearZ)
            {
                //if the sample might need blurring to smooth the blurring transition
                if (blurFalloff)
                {
                    const int blurRadius = MIN(_deepCSpec.blurRadiusFloor - 1, _deepCSpec.blurRadiusFloor - static_cast<int>((_deepCSpec.nearZ - deepFront) / _deepCSpec.nearFalloffRate));
                    //if the calculated blur radius is valid and the sample is still in it's blur radius
                    if ((blurRadius >= 1) && (pixelDistance < (blurRadius + 1)))
                    {
                        pushYSample(currentPixel, isample, channels, _deepCSpec.falloffKernels[blurRadius - 1][pixelDistance], outPixel);
                    }
                }
                //else the sample is not blurred
                continue;
            }
            if (deepFront > _deepCSpec.farZ)
            {
                //if the sample might need blurring to smooth the blurring transition
                if (blurFalloff)
                {
                    const int blurRadius = MIN(_deepCSpec.blurRadiusFloor-1,_deepCSpec.blurRadiusFloor - static_cast<int>((deepFront - _deepCSpec.farZ) / _deepCSpec.farFalloffRate));
                    if ((blurRadius >= 1) && (pixelDistance < (blurRadius + 1)))
                    {
                        pushYSample(currentPixel, isample, channels, _deepCSpec.falloffKernels[blurRadius - 1][pixelDistance], outPixel);
                    }
                }
                //else the sample is not blurred
                continue;
            }
        }

        pushYSample(currentPixel, isample, channels, _deepCSpec.kernel[pixelDistance], outPixel);
    }
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
void BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>::zBlur(const DD::Image::DeepPixel& currentPixel, const DD::Image::ChannelSet& channels, const int pixelDistance, DD::Image::DeepOutPixel& outPixel)
{
    const std::size_t currentSampleCount = currentPixel.getSampleCount();
    for (std::size_t isample = 0; isample < currentSampleCount; ++isample)
    {
        //if only samples in a certain range should be blurred
        if (constrainBlur)
        {
            const float deepFront = currentPixel.getOrderedSample(isample, Chan_DeepFront);
            if (deepFront < _deepCSpec.nearZ)
            {
                //if the sample might need blurring to smooth the blurring transition
                if (blurFalloff)
                {
                    const int blurRadius = _deepCSpec.blurRadiusFloor - static_cast<int>(((_deepCSpec.nearZ - deepFront) / _deepCSpec.nearFalloffRate));
                    pushZSamples(currentPixel, isample, channels, _deepCSpec.volumetricFalloffOffsets[blurRadius - 1], outPixel);
                }
                //else the sample is not blurred
                continue;
            }
            if (deepFront > _deepCSpec.farZ)
            {
                //if the sample might need blurring to smooth the blurring transition
                if (blurFalloff)
                {
                    const int blurRadius = _deepCSpec.blurRadiusFloor - static_cast<int>(((deepFront - _deepCSpec.farZ) / _deepCSpec.farFalloffRate));
                    pushZSamples(currentPixel, isample, channels, _deepCSpec.volumetricFalloffOffsets[blurRadius - 1], outPixel);
                }
                //else the sample is not blurred
                continue;
            }
        }

        pushZSamples(currentPixel, isample, channels, _deepCSpec.volumetricOffsets, outPixel);
    }
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
inline void BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>::pushYSample(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const float kernelValue, DD::Image::DeepOutPixel& outPixel)
{
    foreach(z, channels)
    {
        if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue) || (z == Chan_Alpha))
        {
            outPixel.push_back(currentPixel.getOrderedSample(isample, z) * kernelValue);
        }
        else
        {
            outPixel.push_back(currentPixel.getOrderedSample(isample, z));
        }
    }
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
inline void BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>::pushZSamples(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const std::vector<float>& volumetricOffsets, DD::Image::DeepOutPixel& outPixel)
{
    for (int ivolumetric = -_deepCSpec.zKernelRadius, volumetricOffsetIndex = 0; ivolumetric <= _deepCSpec.zKernelRadius; ++ivolumetric, volumetricOffsetIndex += 2)
    {
        foreach(z, channels)
        {
            if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue) || (z == Chan_Alpha))
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z) * _deepCSpec.zKernel[abs(ivolumetric)]);
            }
            else if (z == Chan_DeepFront)
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z) + volumetricOffsets[volumetricOffsetIndex]);
            }
            else if (z == Chan_DeepBack)
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z) + volumetricOffsets[volumetricOffsetIndex + (volumetricBlur ? 1 : 0)]);
            }
            else
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
            }
        }
    }
}






template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
RawGaussianStrategy<constrainBlur, blurFalloff, volumetricBlur>::RawGaussianStrategy(const DeepCBlurSpec& blurSpec) : BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>(blurSpec)
{
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
RawGaussianStrategy<constrainBlur, blurFalloff, volumetricBlur>::~RawGaussianStrategy()
{
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
inline void RawGaussianStrategy<constrainBlur, blurFalloff, volumetricBlur>::pushXSample(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const float kernelValue, DD::Image::DeepOutPixel& outPixel, float& cumulativeTransparency)
{
    foreach(z, channels)
    {
        if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue) || (z == Chan_Alpha))
        {
            outPixel.push_back(currentPixel.getUnorderedSample(isample, z) * kernelValue);
        }
        else
        {
            outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
        }
    }
}





template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
TransparentModifiedGaussianStrategy<constrainBlur, blurFalloff, volumetricBlur>::TransparentModifiedGaussianStrategy(const DeepCBlurSpec& blurSpec) : BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>(blurSpec)
{
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
TransparentModifiedGaussianStrategy<constrainBlur, blurFalloff, volumetricBlur>::~TransparentModifiedGaussianStrategy()
{
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
inline void TransparentModifiedGaussianStrategy<constrainBlur, blurFalloff, volumetricBlur>::pushXSample(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const float kernelValue, DD::Image::DeepOutPixel& outPixel, float& cumulativeTransparency)
{
    const float blurFactor = cumulativeTransparency * kernelValue;

    foreach(z, channels)
    {
        if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue))
        {
            outPixel.push_back(currentPixel.getOrderedSample(isample, z) * blurFactor);
        }
        else if (z == Chan_Alpha)
        {
            outPixel.push_back(APPROX_ZERO);

            //this in a sense darkens the colour of the sample 
            //where the amount of darkening is dependendent on the samples in front of this sample (in the same pixel of the input image)
            //this mirrors the behaviour that a deep image must be compositied with the 'over operation' before a flat blur can occur
            //therefore in a flat blur the individual samples will have these contributions to the final image
            //compound the darkening effect for samples that have even more samples in front of them
            //decreases the sample alpha to balance out the decrease in RGB so that the un-premultiplied colour remains the same
            //if 'alpha <= 1.0', then '1 - alpha 'will always be a positive float, so darkening can happen indefinitely
            //if 'alpha > 1.0', then '1 - alpha' will be negative and will result in negative RGBAs for samples, which I consider undefined behaviour for this program
            cumulativeTransparency *= (1.0f - currentPixel.getOrderedSample(isample, z));
        }
        else
        {
            outPixel.push_back(currentPixel.getOrderedSample(isample, z));
        }
    }
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
inline void TransparentModifiedGaussianStrategy<constrainBlur, blurFalloff, volumetricBlur>::pushYSample(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const float blurFactor, DD::Image::DeepOutPixel& outPixel)
{
    foreach(z, channels)
    {
        if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue))
        {
            outPixel.push_back(currentPixel.getOrderedSample(isample, z) * blurFactor);
        }
        else if (z == Chan_Alpha)
        {
            outPixel.push_back(APPROX_ZERO);
        }
        else
        {
            outPixel.push_back(currentPixel.getOrderedSample(isample, z));
        }
    }
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
inline void TransparentModifiedGaussianStrategy<constrainBlur, blurFalloff, volumetricBlur>::pushZSamples(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const std::vector<float>& volumetricOffsets, DD::Image::DeepOutPixel& outPixel)
{
    for (int ivolumetric = -_deepCSpec.zKernelRadius, volumetricOffsetIndex = 0; ivolumetric <= _deepCSpec.zKernelRadius; ++ivolumetric, volumetricOffsetIndex += 2)
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
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z) + volumetricOffsets[volumetricOffsetIndex]);
            }
            else if (z == Chan_DeepBack)
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z) + volumetricOffsets[volumetricOffsetIndex + (volumetricBlur ? 1 : 0)]);
            }
            else
            {
                outPixel.push_back(currentPixel.getUnorderedSample(isample, z));
            }
        }
    }
}





template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
ModifiedGaussianStrategy<constrainBlur, blurFalloff, volumetricBlur>::ModifiedGaussianStrategy(const DeepCBlurSpec& blurSpec) : BlurStrategy<constrainBlur, blurFalloff, volumetricBlur>(blurSpec)
{
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
ModifiedGaussianStrategy<constrainBlur, blurFalloff, volumetricBlur>::~ModifiedGaussianStrategy()
{
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur>
inline void ModifiedGaussianStrategy<constrainBlur, blurFalloff, volumetricBlur>::pushXSample(const DD::Image::DeepPixel& currentPixel, const std::size_t isample, const DD::Image::ChannelSet& channels, const float kernelValue, DD::Image::DeepOutPixel& outPixel, float& cumulativeTransparency)
{
    const float blurFactor = cumulativeTransparency * kernelValue;
    foreach(z, channels)
    {
        if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue))
        {
            outPixel.push_back(currentPixel.getOrderedSample(isample, z) * blurFactor);
        }
        else if (z == Chan_Alpha)
        {
            const float alpha = currentPixel.getOrderedSample(isample, z);
            outPixel.push_back(alpha * blurFactor);

            //this in a sense darkens the colour of the sample 
            //where the amount of darkening is dependendent on the samples in front of this sample (in the same pixel of the input image)
            //this mirrors the behaviour that a deep image must be compositied with the 'over operation' before a flat blur can occur
            //therefore in a flat blur the individual samples will have these contributions to the final image
            //compound the darkening effect for samples that have even more samples in front of them
            //decreases the sample alpha to balance out the decrease in RGB so that the un-premultiplied colour remains the same
            //if 'alpha <= 1.0', then '1 - alpha 'will always be a positive float, so darkening can happen indefinitely
            //if 'alpha > 1.0', then '1 - alpha' will be negative and will result in negative RGBAs for samples, which I consider undefined behaviour for this program
            cumulativeTransparency *= (1.0f - alpha);
        }
        else
        {
            outPixel.push_back(currentPixel.getOrderedSample(isample, z));
        }
    }
}