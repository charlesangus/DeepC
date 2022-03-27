#pragma once

#include <utility>
#include <cstdio>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <vector>
#include <array>

#include "SeperableBlurOp.hpp"

const char* const Z_BLUR_OP_CLASS = "ZBlurOp";

template<typename BlurModeStrategyT>
class ZBlurOp : public SeperableBlurOp<BlurModeStrategyT>
{
public:
    ZBlurOp(Node* node, const DeepCBlurSpec& blurSpec) : SeperableBlurOp<BlurModeStrategyT>(node, blurSpec) {}
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
            DeepOutPixel outPixel;

            zBlur(inPlane.getPixel(it), channels, 0, outPixel);

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

template<typename BlurModeStrategyT>
const DD::Image::Op::Description ZBlurOp<BlurModeStrategyT>::d(Z_BLUR_OP_CLASS, "Image/ZBlurOp", buildZBlurOp);