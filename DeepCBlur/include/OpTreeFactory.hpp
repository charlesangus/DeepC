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

template<bool constrainBlur, bool volumetricBlur, template<bool, bool> typename BlurModeStrategyT, typename... Params>
DD::Image::Op* OpTreeFactory_BlurInstance(const BlurInstance blurInstance, Params&&... params)
{
    switch (blurInstance)
    {
        case BlurInstance::X_BLUR: return new XBlurOp<constrainBlur,volumetricBlur,BlurModeStrategyT>(std::forward<Params>(params)...);
        case BlurInstance::Y_BLUR: return new YBlurOp<constrainBlur,volumetricBlur,BlurModeStrategyT>(std::forward<Params>(params)...);
        case BlurInstance::Z_BLUR: return new ZBlurOp<constrainBlur,volumetricBlur,BlurModeStrategyT>(std::forward<Params>(params)...);
        default:return nullptr;
    }
}


template<bool constrainBlur, bool volumetricBlur, typename... Params>
DD::Image::Op* OpTreeFactory_BlurMode(const BlurInstance blurInstance, const int blurMode, Params&&... params)
{
    switch (blurMode)
    {
        case DEEPC_RAW_GAUSSIAN:                   return OpTreeFactory_BlurInstance<constrainBlur, volumetricBlur, RawGaussianStrategy>(blurInstance, std::forward<Params>(params)...);
        case DEEPC_TRANSPARENT_RAW_GAUSSIAN_MODE:  return OpTreeFactory_BlurInstance<constrainBlur, volumetricBlur, TransparentModifiedGaussianStrategy>(blurInstance, std::forward<Params>(params)...);
        case DEEPC_MODIFIED_GAUSSIAN_MODE:         return OpTreeFactory_BlurInstance<constrainBlur, volumetricBlur, ModifiedGaussianStrategy>(blurInstance, std::forward<Params>(params)...);
        default: return nullptr;
    }
}

template<bool constrainBlur, typename... Params>
DD::Image::Op* OpTreeFactory_VolumetricBlur(const BlurInstance blurInstance, const bool volumetricBlur, Params&&... params)
{
    if (volumetricBlur)
    {
        return OpTreeFactory_BlurMode<constrainBlur,true>(blurInstance, std::forward<Params>(params)...);
    }
    else
    {
        return OpTreeFactory_BlurMode<constrainBlur,false>(blurInstance, std::forward<Params>(params)...);
    }
}

template<typename... Params>
DD::Image::Op* OpTreeFactory_ConstrainBlur(const BlurInstance blurInstance, const bool constrainBlur, Params&&... params)
{
    if (constrainBlur)
    {
        return OpTreeFactory_VolumetricBlur<true>(blurInstance, std::forward<Params>(params)...);
    }
    else
    {
        return OpTreeFactory_VolumetricBlur<false>(blurInstance, std::forward<Params>(params)...);
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