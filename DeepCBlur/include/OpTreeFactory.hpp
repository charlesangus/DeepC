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

template<typename... Params>
DD::Image::Op* OpTreeFactory_BlurMode(const int blurMode, const BlurInstance blurInstance, Params&&... params)
{
    switch (blurMode)
    {
        case DEEPC_RAW_GAUSSIAN:                   return OpTreeFactory_BlurInstance<RawGaussianStrategy>(blurInstance, std::forward<Params>(params)...);
        case DEEPC_TRANSPARENT_RAW_GAUSSIAN_MODE:  return OpTreeFactory_BlurInstance<TransparentModifiedGaussianStrategy>(blurInstance, std::forward<Params>(params)...);
        case DEEPC_MODIFIED_GAUSSIAN_MODE:         return OpTreeFactory_BlurInstance<ModifiedGaussianStrategy>(blurInstance, std::forward<Params>(params)...);
        default: return nullptr;
    }
}

template<typename... Params>
DeepCOpTree OpTreeFactory(const bool doZBlur, const int blurMode, Params&&... params)
{
    DeepCOpTree opTree;
    if (!doZBlur)
    {
        opTree.push_back(OpTreeFactory_BlurMode(blurMode, BlurInstance::X_BLUR, std::forward<Params>(params)...));
        opTree.push_back(OpTreeFactory_BlurMode(blurMode, BlurInstance::Y_BLUR, std::forward<Params>(params)...));
    }
    else
    {
        opTree.push_back(OpTreeFactory_BlurMode(blurMode, BlurInstance::X_BLUR, std::forward<Params>(params)...));
        opTree.push_back(OpTreeFactory_BlurMode(blurMode, BlurInstance::Y_BLUR, std::forward<Params>(params)...));
        opTree.push_back(OpTreeFactory_BlurMode(blurMode, BlurInstance::Z_BLUR, std::forward<Params>(params)...));
    }
    return opTree;
}