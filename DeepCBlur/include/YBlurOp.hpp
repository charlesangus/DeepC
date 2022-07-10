#pragma once

#include <cstdio>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <vector>
#include <array>

#include "SeperableBlurOp.hpp"

const char* const Y_BLUR_OP_CLASS = "YBlurOp";

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur, template<bool, bool, bool> class BlurModeStrategyT>
class YBlurOp : public SeperableBlurOp<BlurModeStrategyT<constrainBlur, blurFalloff, volumetricBlur>>
{
public:
    YBlurOp(Node* node, const DeepCBlurSpec& blurSpec) : SeperableBlurOp<BlurModeStrategyT<constrainBlur, blurFalloff, volumetricBlur>>(node, blurSpec) {}
    ~YBlurOp() {};
    const char* Class() const override { return Y_BLUR_OP_CLASS; }
    DD::Image::Op* op() override { return this; }

    void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<DD::Image::RequestData>& requests) override
    {
        DD::Image::DeepOp* input0 = dynamic_cast<DD::Image::DeepOp*>(this->input(0));
        if (input0)
        {
            requests.push_back(DD::Image::RequestData(input0, box, channels, 2 * this->_deepCSpec.blurRadiusFloor + 1));
        }
    }
    bool doDeepEngine(DD::Image::Box box, const DD::Image::ChannelSet& channels, DD::Image::DeepOutputPlane& outPlane) override
    {
        DD::Image::DeepOp* input0 = dynamic_cast<DD::Image::DeepOp*>(this->input(0));
        if (!input0)
        {
            return false;
        }

        outPlane = DD::Image::DeepOutputPlane(channels, box);

        const int maxHeight = this->_deepInfo.format()->height();
        const int maxY = maxHeight - 1;

        DD::Image::Box newBox = { box.x(), box.y(), box.r(), box.t() };

        for (DD::Image::Box::iterator it = box.begin(); it != box.end(); it++)
        {
            DD::Image::DeepOutPixel outPixel;

            int lowerY = it.y - this->_deepCSpec.blurRadiusFloor;
            int upperY = it.y + this->_deepCSpec.blurRadiusFloor;

            //if reflecting pixels above of the image
            if (lowerY < 0)
            {
                //current pixels in the range [lowerY, 0)
                for (int y = -1, reflectedY = 1; y >= lowerY; --y, ++reflectedY)
                {
                    DD::Image::DeepPlane inPlane;
                    newBox.y(reflectedY);
                    newBox.t(reflectedY + 1);
                    if (!input0->deepEngine(newBox, channels, inPlane))
                    {
                        return true;
                    }
                    this->yBlur(inPlane.getPixel(reflectedY, it.x), channels, it.y - y, outPixel);
                }
                lowerY = 0;
            }

            //if need to reflect pixels below of the image
            if (upperY >= maxHeight)
            {
                //current pixels in the range (maxY, upperY]
                for (int y = maxY + 1, reflectedY = maxY - 1; y <= upperY; ++y, --reflectedY)
                {
                    DD::Image::DeepPlane inPlane;
                    newBox.y(reflectedY);
                    newBox.t(reflectedY + 1);
                    if (!input0->deepEngine(newBox, channels, inPlane))
                    {
                        return true;
                    }
                    this->yBlur(inPlane.getPixel(reflectedY, it.x), channels, y - it.y, outPixel);
                }
                upperY = maxY;
            }

            //current pixels in the range [lowerY, upperY]
            for (int y = lowerY; y <= upperY; ++y)
            {
                DD::Image::DeepPlane inPlane;
                newBox.y(y);
                newBox.t(y + 1);
                if (!input0->deepEngine(newBox, channels, inPlane))
                {
                    return true;
                }
                this->yBlur(inPlane.getPixel(y, it.x), channels, abs(it.y - y), outPixel);
            }

            if (constrainBlur)
            {
                DD::Image::DeepPlane inPlane;
                newBox.y(it.y);
                newBox.t(it.y + 1);
                if (!input0->deepEngine(newBox, channels, inPlane))
                {
                    return true;
                }
                DD::Image::DeepPixel targetPixel = inPlane.getPixel(it.y, it.x);
                const std::size_t targetSampleCount = targetPixel.getSampleCount();
                for (std::size_t isample = 0; isample < targetSampleCount; ++isample)
                {
                    const float deepFront = targetPixel.getUnorderedSample(isample, DD::Image::Channel::Chan_DeepFront);
                    //if this sample is not being blurred, then it should not be modified
                    if ((deepFront < this->_deepCSpec.nearZ) || (deepFront > this->_deepCSpec.farZ))
                    {
                        foreach(z, channels)
                        {
                            outPixel.push_back(targetPixel.getUnorderedSample(isample, z));
                        }
                    }
                }
            }

            outPlane.addPixel(outPixel);
        }

        return true;
    }

    static const DD::Image::Op::Description d;
};

static DD::Image::Op* buildYBlurOp(Node* node)
{
    return nullptr;
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur, template<bool, bool, bool> class BlurModeStrategyT>
const DD::Image::Op::Description YBlurOp<constrainBlur, blurFalloff, volumetricBlur, BlurModeStrategyT>::d(Y_BLUR_OP_CLASS, "Image/YBlurOp", buildYBlurOp);
