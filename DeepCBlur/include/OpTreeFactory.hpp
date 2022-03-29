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

template<typename BlurModeStrategyT, typename... Params>
DD::Image::Op* OpTreeFactory_BlurInstance(const BlurInstance blurInstance, Params&&... params)
{
    switch (blurInstance)
    {
        case BlurInstance::X_BLUR: return new XBlurOp<BlurModeStrategyT>(std::forward<Params>(params)...);
        case BlurInstance::Y_BLUR: return new YBlurOp<BlurModeStrategyT>(std::forward<Params>(params)...);
        case BlurInstance::Z_BLUR: return new ZBlurOp<BlurModeStrategyT>(std::forward<Params>(params)...);
        default:return nullptr;
    }
}


template<bool constrainBlur, typename... Params>
DD::Image::Op* OpTreeFactory_BlurMode(const BlurInstance blurInstance, const int blurMode, Params&&... params)
{
    switch (blurMode)
    {
        case DEEPC_RAW_GAUSSIAN:                   return OpTreeFactory_BlurInstance<RawGaussianStrategy<constrainBlur>>(blurInstance, std::forward<Params>(params)...);
        case DEEPC_TRANSPARENT_RAW_GAUSSIAN_MODE:  return OpTreeFactory_BlurInstance<TransparentModifiedGaussianStrategy<constrainBlur>>(blurInstance, std::forward<Params>(params)...);
        case DEEPC_MODIFIED_GAUSSIAN_MODE:         return OpTreeFactory_BlurInstance<ModifiedGaussianStrategy<constrainBlur>>(blurInstance, std::forward<Params>(params)...);
        default: return nullptr;
    }
}

template<typename... Params>
DD::Image::Op* OpTreeFactory_ConstrainBlur(const BlurInstance blurInstance, const bool constrainBlur, Params&&... params)
{
    if (constrainBlur)
    {
        return OpTreeFactory_BlurMode<true>(blurInstance, std::forward<Params>(params)...);
    }
    else
    {
        return OpTreeFactory_BlurMode<false>(blurInstance, std::forward<Params>(params)...);
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