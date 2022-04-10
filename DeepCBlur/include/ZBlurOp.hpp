#pragma once

#include <utility>
#include <cstdio>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <vector>
#include <array>
#include <random>

#include "SeperableBlurOp.hpp"

const char* const Z_BLUR_OP_CLASS = "ZBlurOp";

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur, template<bool, bool, bool> typename BlurModeStrategyT>
class ZBlurOp : public SeperableBlurOp<BlurModeStrategyT<constrainBlur, blurFalloff, volumetricBlur>>
{
public:
    ZBlurOp(Node* node, const DeepCBlurSpec& blurSpec) : SeperableBlurOp<BlurModeStrategyT<constrainBlur, blurFalloff, volumetricBlur>>(node, blurSpec) {}
    ~ZBlurOp() {};
    const char* Class() const override { return Z_BLUR_OP_CLASS; }
    Op* op() override { return this; }
    void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<DD::Image::RequestData>& requests) override
    {
        DeepOp* input0 = dynamic_cast<DeepOp*>(input(0));
        if (input0)
        {
            requests.push_back(RequestData(input0, box, channels, 2 * _deepCSpec.blurRadiusFloor + 1));
        }
    }

    bool doDeepEngine(DD::Image::Box box, const DD::Image::ChannelSet& channels, DD::Image::DeepOutputPlane& outPlane) override
    {
        int id = rand();
        DeepOp* input0 = dynamic_cast<DeepOp*>(input(0));
        if (!input0)
        {
            return false;
        }

        DeepPlane inPlane;
        if (!input0->deepEngine(box, channels, inPlane))
        {
            return true;
        }

        outPlane = DeepOutputPlane(channels, box);

        for (Box::iterator it = box.begin(); it != box.end(); it++)
        {
            //printf("random ID[%d] x[%d] y[%d] box(x:%d,r:%d,y:%d,t:%d)\n",id,it.x,it.y,box.x(),box.r(),box.y(),box.t());
            DeepOutPixel outPixel;

            this->zBlur(inPlane.getPixel(it), channels, 0, outPixel);

            outPlane.addPixel(outPixel);
        }
        
        return true;
    }

    static const Op::Description d;
};

static DD::Image::Op* buildZBlurOp(Node* node)
{
    return nullptr;
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur, template<bool, bool, bool> typename BlurModeStrategyT>
const DD::Image::Op::Description ZBlurOp<constrainBlur, blurFalloff, volumetricBlur, BlurModeStrategyT>::d(Z_BLUR_OP_CLASS, "Image/ZBlurOp", buildZBlurOp);