#include <cstdio>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <vector>
#include <array>

#include "DDImage/DeepOp.h"
#include "DDImage/DDMath.h"
#include "DDImage/Pixel.h"
#include "DDImage/Row.h"
#include "DDImage/DeepComposite.h"

#include "DeepCOp.hpp"
#include "DeepCDefocusSpec.hpp"
#include "DefocusKernels.hpp"

static const char* CLASS = "DeepDefocus";

static const char* const NEAR_DEPTH_OF_FIELD_TEXT = "near DOF";
static const char* const FAR_DEPTH_OF_FIELD_TEXT = "far DOF";
static const char* const BLUR_FILTER_TEXT = "filter";
static const char* const BLUR_QUALITY_TEXT = "quality";
static const char* const BLUR_MODE_TEXT = "mode";
static const char* const MAX_BLUR_SIZE_TEXT = "max blur size";
static const char* const DISTRIBUTION_PROBABILITY_TEXT = "approximation percentage";

static const char* const CIRCLE_DOF_BLUR_NAME = "circular bokeh";
static const char* const BLUR_TYPE_NAMES[] = { CIRCLE_DOF_BLUR_NAME, 0 };

static const char* const EXACT_CIRCLE_NAME = "exact circle";
static const char* const APPROXIMATED_NAME = "approximated circle";
static const char* const BLUR_QUALITY_NAMES[] = { EXACT_CIRCLE_NAME, 0 };

using namespace DD::Image;

static const float INVALID_BLUR_RADIUS = -1.0f;

class DeepDefocus : public CMGDeepOp<DeepCDefocusSpec>
{
private:
	float _maxBlurSize;
	DeepCDefocusSpec _deepSpec;

	bool _recomputeKernels;

	//TODO: check for blurRadius in range [0.0,1.0]
	float getBlurRadius(const float depth) const
	{
		if (depth < _deepSpec.nearDOF)
		{
			return MIN((_deepSpec.nearDOF - depth) * _deepSpec.zScale, _deepSpec.maxBlurRadius);
		}

		if (depth > _deepSpec.farDOF)
		{
			return MIN((depth - _deepSpec.farDOF) * _deepSpec.zScale, _deepSpec.maxBlurRadius);
		}

		return INVALID_BLUR_RADIUS;
	}

public:
	DeepDefocus(Node* node) : 
		CMGDeepOp<DeepCDefocusSpec>(node),
		_maxBlurSize(100.0f),
		_recomputeKernels(true)
	{
		inputs(1);

		_deepSpec.maxBlurRadius = 150.0f; // 100.0 * 1.5
		_deepSpec.maxBlurRadiusCeil = 150;
	}

	const char* node_help() const override
	{
		return "Perfroms a defocus blur on the input deep image";
	}

	const char* Class() const override
	{
		return CLASS;
	}

	Op* op() override
	{
		return this;
	}

	void knobs(Knob_Callback f) override
	{
		Divider(f, "DOF parameters");

		Float_knob(f, &_deepSpec.nearDOF, NEAR_DEPTH_OF_FIELD_TEXT, NEAR_DEPTH_OF_FIELD_TEXT);
		Tooltip(f, "The near DOF bound, any sample with a deep front smaller than this value will be blurred");

		Float_knob(f, &_deepSpec.farDOF, FAR_DEPTH_OF_FIELD_TEXT, FAR_DEPTH_OF_FIELD_TEXT);
		Tooltip(f, "The far DOF bound, any sample with a deep front greater than this value will be blurred");

		Divider(f, "Blur parameters");

		Enumeration_knob(f, &_deepSpec.blurType, BLUR_TYPE_NAMES, BLUR_FILTER_TEXT, BLUR_FILTER_TEXT);
		Tooltip(f, "The type of blur kernel that should be used for blurring");

		Enumeration_knob(f, &_deepSpec.blurQuality, BLUR_QUALITY_NAMES, BLUR_QUALITY_TEXT, BLUR_QUALITY_TEXT);
		Tooltip(f, "The quality of the bokeh kernel\n"
                   "'exact circle' = a circular bokeh kernel"
                   "'approximated circle' = a spiral bokeh kernel that can approximate a circle to varying degrees");

		Float_knob(f, &_maxBlurSize, MAX_BLUR_SIZE_TEXT, MAX_BLUR_SIZE_TEXT);
		Tooltip(f, "The maximum blur size of a DOF kernel");

		Float_knob(f, &_deepSpec.distribution_probability, DISTRIBUTION_PROBABILITY_TEXT, DISTRIBUTION_PROBABILITY_TEXT);
		Tooltip(f, "The degree to which a circular bokeh filter should be approximated (0.1 = 10% , 1.0 = 100%)\n"
                   "the approximations reduce the pixels that make up the circular bokeh filter, to make the node faster\n"
                   "note that a 1.0(100%) approximation is not the same as a circular bokeh filter, but very close");
		SetRange(f, 0.1f, 1.0f);
		if (_deepSpec.blurQuality != CMG_APPROXIMATED_CIRCLE)
		{
			SetFlags(f, Knob::HIDDEN | Knob::FORCE_RANGE);
		}
		else
		{
			SetFlags(f, Knob::FORCE_RANGE);
		}

		zBlurTab(f);
		//sampleOptimisationTab(f);
	}

	int knob_changed(DD::Image::Knob* k) override
	{
		if ((_deepSpec.blurQuality != CMG_APPROXIMATED_CIRCLE) && knob(DISTRIBUTION_PROBABILITY_TEXT)->isVisible())
		{
			knob(DISTRIBUTION_PROBABILITY_TEXT)->hide();
		}
		else if ((_deepSpec.blurQuality == CMG_APPROXIMATED_CIRCLE) && !knob(DISTRIBUTION_PROBABILITY_TEXT)->isVisible())
		{
			knob(DISTRIBUTION_PROBABILITY_TEXT)->show();
		}
		
		_recomputeKernels = true;
		return 1;
	}

	void _open() override
	{
		if (_recomputeKernels)
		{
			_deepSpec.maxBlurRadius = _maxBlurSize * 1.5f;
			_deepSpec.maxBlurRadiusCeil = static_cast<int>(ceil(_deepSpec.maxBlurRadius));

			_deepSpec.metadataKernel = std::move(getCircleMetadataKernel(_deepSpec.maxBlurRadiusCeil));

			auto kernelFunction = _deepSpec.getKernelFunction();
			if (kernelFunction == nullptr)
			{
				Op::error("failed to create CMGDefocusBlur blur kernels");
				return;
			}
#if 1
			_deepSpec.kernels.resize(_deepSpec.maxBlurRadiusCeil);
			for (int radius = 1; radius <= _deepSpec.maxBlurRadiusCeil; ++radius)
			{
				_deepSpec.kernels[radius - 1] = std::move(kernelFunction(radius, 1.0f, _deepSpec.metadataKernel));
			}
#else
			_deepSpec.DOFKernels.resize(_deepSpec.maxBlurRadiusCeil);
			for (int radius = 1; radius < _deepSpec.maxBlurRadiusCeil; ++radius)
			{
				_deepSpec.DOFKernels[radius - 1] = std::move(kernelFunction(radius, _deepSpec.distribution_probability, _deepSpec.metadataKernel));
			}
#endif

			_recomputeKernels = false;
		}
	}

	bool test_input(int idx, Op* op) const
	{
		switch (idx)
		{
			case 0: return dynamic_cast<DeepOp*>(op);
			default: return false;
		}
	}

	void _validate(bool for_real)
	{
		DeepOp* input0 = dynamic_cast<DeepOp*>(input(0));
		if (!input0) {
			_deepInfo = DeepInfo();
			return;
		}

		input0->validate(for_real);
		_deepInfo = input0->deepInfo();
	}

	void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests) override
	{
		DeepOp* input0 = dynamic_cast<DeepOp*>(input(0));
		if (input0)
		{
			const int blurRowAmount = MIN(_deepInfo.format()->height(), box.t() + _deepSpec.maxBlurRadiusCeil) + MAX(0, box.y() - _deepSpec.maxBlurRadiusCeil) + 1;
			const Box newBox = {
				MAX(0, box.x() - _deepSpec.maxBlurRadiusCeil), 
				box.y(), 
				MIN(_deepInfo.format()->width(), box.r() + _deepSpec.maxBlurRadiusCeil), 
				box.t()
			};
			requests.push_back(RequestData(input0, newBox, channels, blurRowAmount));
		}
	}

	bool doDeepEngine(DD::Image::Box box, const ChannelSet& channels, DeepOutputPlane& outPlane) override
	{
		DeepOp* input0 = dynamic_cast<DeepOp*>(input(0));
		if (!input0)
		{
			return true;
		}

		outPlane = DeepOutputPlane(channels, box);

		const int maxWidth = _deepInfo.format()->width();
		const int maxX = maxWidth - 1;
		const int maxHeight = _deepInfo.format()->height();
		const int maxY = maxHeight - 1;

		for (Box::iterator it = box.begin(); it != box.end(); it++)
		{
			DeepOutPixel outPixel;

			Box newBox = {
				MAX(0, box.x() - _deepSpec.maxBlurRadiusCeil),
				box.y(),
				MIN(_deepInfo.format()->width(), box.r() + _deepSpec.maxBlurRadiusCeil),
				box.t()
			};

			int upperX = MIN(maxX, it.x + _deepSpec.maxBlurRadiusCeil);
			int lowerX = MAX(0, it.x - _deepSpec.maxBlurRadiusCeil);
			int upperY = MIN(maxY, it.y - _deepSpec.maxBlurRadiusCeil);
			int lowerY = MAX(0, it.y + _deepSpec.maxBlurRadiusCeil);
			
			for (int y = lowerY; y <= upperY; ++y)
			{
				DeepPlane currentPlane;
				newBox.y(y);
				newBox.t(y + 1);
				if (!input0->deepEngine(newBox, channels, currentPlane))
				{
					return true;
				}

				const int yOff = y - it.y;
				for (int x = lowerX; x <= upperX; ++x)
				{
					const DeepPixel currentPixel = currentPlane.getPixel(y, x);
					const std::size_t currentSampleCount = currentPixel.getSampleCount();

					for (std::size_t currentSample = 0; currentSample < currentSampleCount; ++currentSample)
					{
						const float currentBlurRadius = getBlurRadius(currentPixel.getUnorderedSample(currentSample, Chan_DeepFront));
						const int currentBlurRadiusCeil = static_cast<int>(ceilf(currentBlurRadius));

						//if the current sample is within the DOF
						if (currentBlurRadius == INVALID_BLUR_RADIUS)
						{
							//do not blur it
							continue;
						}

						const int xOff = x - it.x;
						//map x/yOff from [-radius,radius] -> x/y [0,2*radius]
						const std::size_t kernelIndex = static_cast<std::size_t>((yOff + currentBlurRadiusCeil) * (2 * currentBlurRadiusCeil + 1) + (xOff + currentBlurRadiusCeil));
						const float currentIntensity = _deepSpec.kernels[currentBlurRadiusCeil - 1][kernelIndex];
						if (currentIntensity != 0.0f)
						{							
							static_assert(Chan_Red == 1, "The red channel must be value 1 in the Channel enum");
							static_assert(Chan_Green == 2, "The green channel must be value 2 in the Channel enum");
							static_assert(Chan_Blue == 3, "The blue channel must be value 3 in the Channel enum");
							static_assert(Chan_Alpha == 4, "The alpha channel must be value 4 in the Channel enum");
							foreach(z, channels)
							{
								if ((z >= 1) && (z <= 4))
								{
									outPixel.push_back(currentPixel.getUnorderedSample(currentSample, z) * currentIntensity);
								}
								else
								{
									outPixel.push_back(currentPixel.getUnorderedSample(currentSample, z));
								}
							}
						}
					}
				}
			}

			//the samples not blurred need to be added back in
			DeepPlane currentPlane;
			newBox.y(it.y);
			newBox.t(it.y + 1);
			if (!input0->deepEngine(newBox, channels, currentPlane))
			{
				return true;
			}

			const DeepPixel currentPixel = currentPlane.getPixel(it);
			const std::size_t currentSampleCount = currentPixel.getSampleCount();
			for (std::size_t currentSample = 0; currentSample < currentSampleCount; ++currentSample)
			{
				const float currentDepth = currentPixel.getUnorderedSample(currentSample, Chan_DeepFront);
				if ((currentDepth >= _deepSpec.nearDOF) && (currentDepth <= _deepSpec.farDOF))
				{
					foreach(z, channels)
					{
						outPixel.push_back(currentPixel.getUnorderedSample(currentSample, z));
					}
				}
			}

			outPlane.addPixel(outPixel);
		}

		return true;
	}

	static const Description d;
};


static Op* build(Node* node)
{
	return new DeepDefocus(node);
}

const Op::Description DeepDefocus::d(::CLASS, "Image/DeepDefocus", build);