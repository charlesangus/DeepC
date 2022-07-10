#pragma once

#include <algorithm>
#include <vector>
#include <array>

#include "SeperableBlurOp.hpp"

const char* const X_BLUR_OP_CLASS = "XBlurOp";

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur, template<bool, bool, bool> class BlurModeStrategyT>
class XBlurOp : public SeperableBlurOp<BlurModeStrategyT<constrainBlur, blurFalloff, volumetricBlur>>
{
public:
    XBlurOp(Node* node, const DeepCBlurSpec& blurSpec) : SeperableBlurOp<BlurModeStrategyT<constrainBlur, blurFalloff, volumetricBlur>>(node,blurSpec){}
    ~XBlurOp() {};
    const char* Class() const override{ return X_BLUR_OP_CLASS; }
    DD::Image::Op* op() override { return this; }

    void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<DD::Image::RequestData>& requests) override
    {
        DD::Image::DeepOp* input0 = dynamic_cast<DD::Image::DeepOp*>(this->input(0));
        if (input0)
        {
            requests.push_back(DD::Image::RequestData(input0, { MAX(0, box.x() - this->_deepCSpec.blurRadiusFloor),
                                                     box.y(),
                                                     MIN(this->_deepInfo.format()->width(), box.r() + this->_deepCSpec.blurRadiusFloor),
                                                     box.t() }, 
                                                     channels, 
                                                     2 * this->_deepCSpec.blurRadiusFloor + 1));
        }
    }
    bool doDeepEngine(DD::Image::Box box, const DD::Image::ChannelSet& channels, DD::Image::DeepOutputPlane& outPlane) override
    {
        DD::Image::DeepOp* input0 = dynamic_cast<DD::Image::DeepOp*>(this->input(0));
        if (!input0)
        {
            return false;
        }

        const int maxWidth = this->_deepInfo.format()->width();
        const int maxX = maxWidth - 1;

        DD::Image::DeepPlane inPlane;
        DD::Image::Box newBox = { MAX(0, box.x() - this->_deepCSpec.blurRadiusFloor),
                       box.y(),
                       MIN(maxWidth, box.r() + this->_deepCSpec.blurRadiusFloor),
                       box.t() };
        if (!input0->deepEngine(newBox, channels, inPlane))
        {
            return true;
        }

        outPlane = DD::Image::DeepOutputPlane(channels, box);

        //current pixel is the pixel currently spreading it's intensity to the target pixel
        //target pixel is the outPixel currently being constructed

        for (DD::Image::Box::iterator it = box.begin(); it != box.end(); it++)
        {
            DD::Image::DeepOutPixel outPixel;

            //reflects the image when trying to access pixels outside the image
            //NOTE: algorithm does not support when radius of blur is greater than the image size, as the calculated reflected pixels would buffer overflow

            int lowerX = it.x - this->_deepCSpec.blurRadiusFloor;
            int upperX = it.x + this->_deepCSpec.blurRadiusFloor;

            //if reflecting pixels to the left of the image
            if (lowerX < 0)
            {
                //current pixels in the range [lowerX, 0)
                for (int x = -1, reflectedX = 1; x >= lowerX; --x, ++reflectedX)
                {
                    this->xBlur(inPlane.getPixel(it.y, reflectedX), channels, it.x - x, outPixel);
                }
                lowerX = 0;
            }

            //if reflecting pixels to the right of the image
            if (upperX >= maxWidth)
            {
                //current pixels in the range (maxX, upperX]
                for (int x = maxX + 1, reflectedX = maxX - 1; x <= upperX; ++x, --reflectedX)
                {
                    this->xBlur(inPlane.getPixel(it.y, reflectedX), channels, x - it.x, outPixel);
                }
                upperX = maxX;
            }

            //current pixels in the range [lowerX, upperX]
            for (int x = lowerX; x <= upperX; ++x)
            {
                this->xBlur(inPlane.getPixel(it.y, x), channels, abs(it.x - x), outPixel);
            }

            if (constrainBlur)
            {
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

static DD::Image::Op* buildXBlurOp(Node* node)
{
    return nullptr;
}

template<bool constrainBlur, bool blurFalloff, bool volumetricBlur, template<bool, bool, bool> class BlurModeStrategyT>
const DD::Image::Op::Description XBlurOp<constrainBlur, blurFalloff, volumetricBlur, BlurModeStrategyT>::d(X_BLUR_OP_CLASS, "Image/XBlurOp", buildXBlurOp);
