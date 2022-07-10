#pragma once

#include "DDImage/DeepOp.h"
#include "DDImage/Knobs.h"

#include "DeepCDebugging.hpp"

template<typename DeepSpecT>
class DeepCOp : public DD::Image::DeepOnlyOp
{
protected:
    DeepSpecT _deepCSpec;

    bool _dropHiddenSamples;

    bool _dropRedundantSamples;
    float _minimumColourThreshold;
    float _minimumAlphaThreshold;

    bool _mergeCloseSamples;
    float _minimumDistanceThreshold;

    DeepCOp(Node* node);
    virtual ~DeepCOp();

    int knob_changed(DD::Image::Knob* k) override;

    void advancedBlurParameters(DD::Image::Knob_Callback f);
    void sampleOptimisationTab(DD::Image::Knob_Callback f);
};

#include "../src/DeepCOp.cpp"
