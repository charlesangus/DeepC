#pragma once

#include <cstdio>
#include <iostream>
#include <algorithm>
#include <vector>
#include <array>

#include "DDImage/DeepOp.h"
#include "DDImage/Pixel.h"
#include "DDImage/DDMath.h"
#include "DDImage/Knobs.h"

#include "DeepCBlurSpec.hpp"
#include "DeepCDebugging.hpp"

//'BlurModeStrategyT' must be a subclass of 'BlurStrategy'
template<class BlurModeStrategyT>
class SeperableBlurOp : public DD::Image::DeepOnlyOp, public BlurModeStrategyT
{
    protected:
        SeperableBlurOp(Node* node, const DeepCBlurSpec& blurSpec) : DeepOnlyOp(node), BlurModeStrategyT(blurSpec)
        {
            inputs(1);
        }
        virtual ~SeperableBlurOp()
        {
        }
    
    public:
        bool test_input(int idx, DD::Image::Op* op) const override
        {
            switch (idx)
            {
                case 0: return dynamic_cast<DD::Image::DeepOp*>(op);
                default: return false;
            }
        }
        void _validate(bool for_real) override
        {
            DD::Image::DeepOp* input0 = dynamic_cast<DD::Image::DeepOp*>(input(0));
            if (!input0) {
                this->_deepInfo = DD::Image::DeepInfo();
                return;
            }

            input0->validate(for_real);
            this->_deepInfo = input0->deepInfo();
        }
        const char* node_help() const override
        {
            return "Performs a 1D blur on it's deep input";
        }
};
