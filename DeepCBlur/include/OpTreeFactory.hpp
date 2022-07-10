#pragma once

#include "DeepCBlurSpec.hpp"
#include "XBlurOp.hpp"
#include "YBlurOp.hpp"
#include "ZBlurOp.hpp"
#include "DeepCOpTree.hpp"
#include "BlurStrategy.hpp"

enum class BlurInstance
{
    X_BLUR,
    Y_BLUR,
    Z_BLUR,
    UNKNOWN
};

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur, template<bool, bool, bool> class BlurModeStrategyT, typename... Params>
DD::Image::Op* OpTreeFactory_BlurInstance(const BlurInstance blurInstance, Params&&... params)
{
    switch (blurInstance)
    {
        case BlurInstance::X_BLUR: return new XBlurOp<constrainBlur, blurFalloff, volumetricBlur, BlurModeStrategyT>(std::forward<Params>(params)...);
        case BlurInstance::Y_BLUR: return new YBlurOp<constrainBlur, blurFalloff, volumetricBlur, BlurModeStrategyT>(std::forward<Params>(params)...);
        case BlurInstance::Z_BLUR: return new ZBlurOp<constrainBlur, blurFalloff, volumetricBlur, BlurModeStrategyT>(std::forward<Params>(params)...);
        default:return nullptr;
    }
}


template<bool constrainBlur, bool blurFalloff, bool volumetricBlur, typename... Params>
DD::Image::Op* OpTreeFactory_BlurMode(const BlurInstance blurInstance, const int blurMode, Params&&... params)
{
    switch (blurMode)
    {
        case DEEPC_RAW_GAUSSIAN:                   return OpTreeFactory_BlurInstance<constrainBlur, blurFalloff, volumetricBlur, RawGaussianStrategy>(blurInstance, std::forward<Params>(params)...);
        case DEEPC_TRANSPARENT_RAW_GAUSSIAN_MODE:  return OpTreeFactory_BlurInstance<constrainBlur, blurFalloff, volumetricBlur, TransparentModifiedGaussianStrategy>(blurInstance, std::forward<Params>(params)...);
        case DEEPC_MODIFIED_GAUSSIAN_MODE:         return OpTreeFactory_BlurInstance<constrainBlur, blurFalloff, volumetricBlur, ModifiedGaussianStrategy>(blurInstance, std::forward<Params>(params)...);
        default: return nullptr;
    }
}

template<bool constrainBlur, bool blurFalloff, typename... Params>
DD::Image::Op* OpTreeFactory_VolumetricBlur(const BlurInstance blurInstance, const bool volumetricBlur, Params&&... params)
{
    if (volumetricBlur)
    {
        return OpTreeFactory_BlurMode<constrainBlur, blurFalloff, true>(blurInstance, std::forward<Params>(params)...);
    }
    else
    {
        return OpTreeFactory_BlurMode<constrainBlur, blurFalloff, false>(blurInstance, std::forward<Params>(params)...);
    }
}

template<bool constrainBlur, typename... Params>
DD::Image::Op* OpTreeFactory_blurFalloff(const BlurInstance blurInstance, const bool blurFalloff, Params&&... params)
{
    if (blurFalloff)
    {
        return OpTreeFactory_VolumetricBlur<constrainBlur,true>(blurInstance, std::forward<Params>(params)...);
    }
    else
    {
        return OpTreeFactory_VolumetricBlur<constrainBlur,false>(blurInstance, std::forward<Params>(params)...);
    }
}

template<typename... Params>
DD::Image::Op* OpTreeFactory_ConstrainBlur(const BlurInstance blurInstance, const bool constrainBlur, Params&&... params)
{
    if (constrainBlur)
    {
        return OpTreeFactory_blurFalloff<true>(blurInstance, std::forward<Params>(params)...);
    }
    else
    {
        return OpTreeFactory_blurFalloff<false>(blurInstance, std::forward<Params>(params)...);
    }
}

template<typename... Params>
DeepCOpTree OpTreeFactory(const bool doZBlur, Params&&... params)
{
    DeepCOpTree opTree;
    if (!doZBlur)
    {
        opTree.push_back(OpTreeFactory_ConstrainBlur(BlurInstance::X_BLUR, std::forward<Params>(params)...));
        opTree.push_back(OpTreeFactory_ConstrainBlur(BlurInstance::Y_BLUR, std::forward<Params>(params)...));
    }
    else
    {
        opTree.push_back(OpTreeFactory_ConstrainBlur(BlurInstance::X_BLUR, std::forward<Params>(params)...));
        opTree.push_back(OpTreeFactory_ConstrainBlur(BlurInstance::Y_BLUR, std::forward<Params>(params)...));
        opTree.push_back(OpTreeFactory_ConstrainBlur(BlurInstance::Z_BLUR, std::forward<Params>(params)...));
    }
    return opTree;
}
