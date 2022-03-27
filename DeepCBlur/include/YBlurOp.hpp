#pragma once

#include <cstdio>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <vector>
#include <array>

#include "SeperableBlurOp.hpp"

const char* const Y_BLUR_OP_CLASS = "YBlurOp";

template<typename BlurModeStrategyT>
class YBlurOp : public SeperableBlurOp<BlurModeStrategyT>
{
public:
	YBlurOp(Node* node, const DeepCBlurSpec& blurSpec) : SeperableBlurOp<BlurModeStrategyT>(node, blurSpec) {}
	~YBlurOp() {};
	const char* Class() const override { return Y_BLUR_OP_CLASS; }
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

		outPlane = DeepOutputPlane(channels, box);

		const int maxHeight = _deepInfo.format()->height();
		const int maxY = maxHeight - 1;

		for (Box::iterator it = box.begin(); it != box.end(); it++)
		{
			DeepOutPixel outPixel;

			int lowerY = it.y - _deepCSpec.blurRadiusFloor;
			int upperY = it.y + _deepCSpec.blurRadiusFloor;

			//if reflecting pixels above of the image
			if (lowerY < 0)
			{
				//current pixels in the range [lowerY, 0)
				for (int y = -1, reflectedY = 1; y >= lowerY; --y, ++reflectedY)
				{
					DeepPlane inPlane;
					if (!input0->deepEngine({ box.x(), reflectedY, box.r(), reflectedY + 1 }, channels, inPlane))
					{
						return true;
					}
					yBlur(inPlane.getPixel(reflectedY, it.x), channels, it.y - y, outPixel);
				}
				lowerY = 0;
			}

			//if need to reflect pixels below of the image
			if (upperY >= maxHeight)
			{
				//current pixels in the range (maxY, upperY]
				for (int y = maxY + 1, reflectedY = maxY - 1; y <= upperY; ++y, --reflectedY)
				{
					DeepPlane inPlane;
					if (!input0->deepEngine({ box.x(), reflectedY, box.r(), reflectedY + 1 }, channels, inPlane))
					{
						return true;
					}
					yBlur(inPlane.getPixel(reflectedY, it.x), channels, y - it.y, outPixel);
				}
				upperY = maxY;
			}

			//current pixels in the range [lowerY, upperY]
			for (int y = lowerY; y <= upperY; ++y)
			{
				DeepPlane inPlane;
				if (!input0->deepEngine({ box.x(), y, box.r(), y + 1 }, channels, inPlane))
				{
					return true;
				}
				yBlur(inPlane.getPixel(y, it.x), channels, abs(it.y - y), outPixel);
			}

			outPlane.addPixel(outPixel);
		}

		return true;
	}

	static const Op::Description d;
};

static DD::Image::Op* buildYBlurOp(Node* node)
{
	return nullptr;
}

template<typename BlurModeStrategyT>
const DD::Image::Op::Description YBlurOp<BlurModeStrategyT>::d(Y_BLUR_OP_CLASS, "Image/YBlurOp", buildYBlurOp);