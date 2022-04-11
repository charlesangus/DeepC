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

static const char* CLASS = "DeepCDefocus";

static const char* const NEAR_DEPTH_OF_FIELD_TEXT = "near DOF";
static const char* const NEAR_DEPTH_OF_FIELD_RATE_TEXT = "near DOF rate";
static const char* const FAR_DEPTH_OF_FIELD_TEXT = "far DOF";
static const char* const FAR_DEPTH_OF_FIELD_RATE_TEXT = "far DOF rate";
static const char* const BLUR_FILTER_TEXT = "filter";
static const char* const BLUR_QUALITY_TEXT = "quality";
static const char* const BLUR_MODE_TEXT = "mode";
static const char* const MAX_BLUR_SIZE_TEXT = "max blur size";
static const char* const DISTRIBUTION_PROBABILITY_TEXT = "approximation";

static const char* const CIRCLE_DOF_BLUR_NAME = "circular bokeh";
static const char* const BLUR_TYPE_NAMES[] = { CIRCLE_DOF_BLUR_NAME, 0 };

static const char* const EXACT_CIRCLE_NAME = "exact circle";
static const char* const APPROXIMATED_NAME = "approximated circle";
static const char* const BLUR_QUALITY_NAMES[] = { EXACT_CIRCLE_NAME, 0 };

using namespace DD::Image;

static const float INVALID_BLUR_RADIUS = -1.0f;

class DeepCDefocus : public DeepCOp<DeepCDefocusSpec>
{
private:
	float _maxBlurSize;
	DeepCDefocusSpec _deepCSpec;

	bool _recomputeKernels;

	//TODO: check for blurRadius in range [0.0,1.0]
	float getBlurRadius(const float depth) const
	{
		if (depth < _deepCSpec.nearDOF)
		{
			return MIN((_deepCSpec.nearDOF - depth) * _deepCSpec.nearDOFRate, _deepCSpec.maxBlurRadius);
		}

		if (depth > _deepCSpec.farDOF)
		{
			return MIN((depth - _deepCSpec.farDOF) * _deepCSpec.farDOFRate, _deepCSpec.maxBlurRadius);
		}

		return INVALID_BLUR_RADIUS;
	}

public:
	DeepCDefocus(Node* node) : 
		DeepCOp<DeepCDefocusSpec>(node),
		_maxBlurSize(100.0f),
		_recomputeKernels(true)
	{
		inputs(1);

		bool hasComputedKernels = _deepCSpec.init(_maxBlurSize);
		if (!hasComputedKernels)
		{
			Op::error("failed to create DeepCDefocus blur kernels");
			return;
		}
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

		Float_knob(f, &_deepCSpec.nearDOF, NEAR_DEPTH_OF_FIELD_TEXT, NEAR_DEPTH_OF_FIELD_TEXT);
		Tooltip(f, "The near DOF plane, any sample with a deep front smaller than this value will be blurred");

		Float_knob(f, &_deepCSpec.nearDOFRate, NEAR_DEPTH_OF_FIELD_RATE_TEXT, NEAR_DEPTH_OF_FIELD_RATE_TEXT);
		Tooltip(f, "the rate at which the blur kernel increases in blur size before the near DOF plane");

		Float_knob(f, &_deepCSpec.farDOF, FAR_DEPTH_OF_FIELD_TEXT, FAR_DEPTH_OF_FIELD_TEXT);
		Tooltip(f, "The far DOF plane, any sample with a deep front greater than this value will be blurred");

		Float_knob(f, &_deepCSpec.farDOFRate, FAR_DEPTH_OF_FIELD_RATE_TEXT, FAR_DEPTH_OF_FIELD_RATE_TEXT);
		Tooltip(f, "the rate at which the blur kernel increase in blur size beyond the far DOF plane");

		Divider(f, "Blur parameters");

		Enumeration_knob(f, &_deepCSpec.blurType, BLUR_TYPE_NAMES, BLUR_FILTER_TEXT, BLUR_FILTER_TEXT);
		Tooltip(f, "The type of blur kernel that should be used for blurring");

		Enumeration_knob(f, &_deepCSpec.blurQuality, BLUR_QUALITY_NAMES, BLUR_QUALITY_TEXT, BLUR_QUALITY_TEXT);
		Tooltip(f, "The quality of the bokeh kernel\n"
                   "'exact circle' = a circular bokeh kernel"
                   "'approximated circle' = a spiral bokeh kernel that can approximate a circle to varying degrees");

		Float_knob(f, &_maxBlurSize, MAX_BLUR_SIZE_TEXT, MAX_BLUR_SIZE_TEXT);
		Tooltip(f, "The maximum blur size of a DOF kernel");

		Float_knob(f, &_deepCSpec.distribution_probability, DISTRIBUTION_PROBABILITY_TEXT, DISTRIBUTION_PROBABILITY_TEXT);
		Tooltip(f, "The degree to which a circular bokeh filter should be approximated (0.1 = 10% , 1.0 = 100%)\n"
                   "the approximations reduce the pixels that make up the bokeh filter, to make the node faster\n"
                   "note that a 1.0(100%) approximation is not the same as a circular bokeh filter, but is close");
		SetRange(f, 0.1f, 1.0f);
		if (_deepCSpec.blurQuality != DEEPC_APPROXIMATED_CIRCLE)
		{
			SetFlags(f, Knob::HIDDEN | Knob::FORCE_RANGE);
		}
		else
		{
			SetFlags(f, Knob::FORCE_RANGE);
		}

		//zBlurTab(f);
		//sampleOptimisationTab(f);
	}

	int knob_changed(DD::Image::Knob* k) override
	{
		if ((_deepCSpec.blurQuality != DEEPC_APPROXIMATED_CIRCLE) && knob(DISTRIBUTION_PROBABILITY_TEXT)->isVisible())
		{
			knob(DISTRIBUTION_PROBABILITY_TEXT)->hide();
		}
		else if ((_deepCSpec.blurQuality == DEEPC_APPROXIMATED_CIRCLE) && !knob(DISTRIBUTION_PROBABILITY_TEXT)->isVisible())
		{
			knob(DISTRIBUTION_PROBABILITY_TEXT)->show();
		}
		
		if (k->is(MAX_BLUR_SIZE_TEXT))
		{
			_recomputeKernels = true;
		}
		return 1;
	}

	void _open() override
	{
		if (_recomputeKernels)
		{

			bool hasComputedKernels = _deepCSpec.init(_maxBlurSize);
			if (!hasComputedKernels)
			{
				Op::error("failed to create DeepCDefocus blur kernels");
				return;
			}

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
			const Box newBox = {
				MAX(0, box.x() - _deepCSpec.maxBlurRadiusCeil), 
				box.y(), 
				MIN(_deepInfo.format()->width(), box.r() + _deepCSpec.maxBlurRadiusCeil), 
				box.t()
			};
			requests.push_back(RequestData(input0, newBox, channels, _deepCSpec.maxBlurRadiusCeil * 2 + 1));
		}
	}

	inline void pushSample(const DeepPixel& currentPixel, const std::size_t isample, const ChannelSet& channels, const float blurFactor, const float oldAlpha, const float newAlpha, DD::Image::DeepOutPixel& outPixel)
	{
		foreach(z, channels)
		{
			if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue))
			{
				outPixel.push_back(currentPixel.getOrderedSample(isample, z) * blurFactor);
			}
			else if (z == Chan_Alpha)
			{
				outPixel.push_back(newAlpha);
			}
			else
			{
				outPixel.push_back(currentPixel.getOrderedSample(isample, z));
			}
		}
	}

	inline void pushSample(const DeepPixel& currentPixel, const std::size_t isample, const ChannelSet& channels, const float blurFactor, const float oldAlpha, DD::Image::DeepOutPixel& outPixel)
	{
		foreach(z, channels)
		{
			if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue))
			{
				outPixel.push_back(currentPixel.getOrderedSample(isample, z) * blurFactor);
			}
			else if (z == Chan_Alpha)
			{
				outPixel.push_back(oldAlpha * blurFactor);
			}
			else
			{
				outPixel.push_back(currentPixel.getOrderedSample(isample, z));
			}
		}
	}

	inline bool blurSamples(DD::Image::Box box, const ChannelSet& channels, DeepOp* input0, DeepOutputPlane& outPlane)
	{
		const int maxWidth = _deepInfo.format()->width();
		const int maxX = maxWidth - 1;
		const int maxHeight = _deepInfo.format()->height();
		const int maxY = maxHeight - 1;

		Box newBox = {
			MAX(0, box.x() - _deepCSpec.maxBlurRadiusCeil),
			box.y(),
			MIN(_deepInfo.format()->width(), box.r() + _deepCSpec.maxBlurRadiusCeil),
			box.t()
		};

		DeepPlane currentBoxPlane;
		int currentBoxY = -1;

		for (Box::iterator it = box.begin(); it != box.end(); it++)
		{
			DeepOutPixel outPixel;

			int upperX = MIN(maxX, it.x + _deepCSpec.maxBlurRadiusCeil);
			int lowerX = MAX(0, it.x - _deepCSpec.maxBlurRadiusCeil);
			int upperY = MIN(maxY, it.y + _deepCSpec.maxBlurRadiusCeil);
			int lowerY = MAX(0, it.y - _deepCSpec.maxBlurRadiusCeil);

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
				const int yDistance = abs(yOff);
				for (int x = lowerX; x <= upperX; ++x)
				{
					const DeepPixel currentPixel = currentPlane.getPixel(y, x);
					const std::size_t currentSampleCount = currentPixel.getSampleCount();
					float cumulativeTransparency = 1.0f;

					for (std::size_t currentSample = currentSampleCount; currentSample-- > 0;)
					{
						//if the current sample is not visible in the original image, then it should not be blurred
						if (cumulativeTransparency == 0.0f)
						{
							break;
						}

						const float currentBlurRadius = getBlurRadius(currentPixel.getOrderedSample(currentSample, Chan_DeepFront));
						const int currentBlurRadiusCeil = static_cast<int>(ceilf(currentBlurRadius));
						const float alpha = currentPixel.getOrderedSample(currentSample, Chan_Alpha);
						//if the current sample is within the DOF, or it is blurred but does not reach the target pixel
						if ((currentBlurRadius == INVALID_BLUR_RADIUS) || (yDistance > currentBlurRadiusCeil))
						{
							//do not blur it
							//cumulativeTransparency *= (1.0f - alpha);
							continue;
						}

						std::size_t kernelIndex = static_cast<std::size_t>(currentBlurRadiusCeil - 1);
						auto kernelOffsetItr = _deepCSpec.kernels[kernelIndex].find({ x - it.x, yOff });

						//if the current sample's pixel is not in the kernel
						if (kernelOffsetItr == _deepCSpec.kernels[kernelIndex].end())
						{
							//do not blur it
							//cumulativeTransparency *= (1.0f - alpha);
							continue;
						}

						const float intensity = kernelOffsetItr->isEdgePixel ?
							getEdgeIntensity(currentBlurRadius, _deepCSpec.metadataKernel) :
							getInnerIntensity(currentBlurRadius, _deepCSpec.metadataKernel);

						//in case the same pixel is in the kernel more than once, the blur factor is multiplied by the kernel count
						//std::size_t count = _deepCSpec.kernels[kernelIndex].count({ x - it.x, yOff });
						pushSample(currentPixel, currentSample, channels, cumulativeTransparency * intensity, alpha, outPixel);
						//cumulativeTransparency *= (1.0f - alpha);
					}
				}
			}

			//for every new row of the box get the deep plane of the new row
			if (it.y != currentBoxY)
			{
				currentBoxPlane = DeepPlane();
				if (!input0->deepEngine({ box.x(),it.y,box.r(),it.y + 1 }, channels, currentBoxPlane))
				{
					return true;
				}
				currentBoxY = it.y;
			}

			//the samples not blurred need to be added back in
			const DeepPixel currentPixel = currentBoxPlane.getPixel(it);
			const std::size_t currentSampleCount = currentPixel.getSampleCount();
			for (std::size_t currentSample = 0; currentSample < currentSampleCount; ++currentSample)
			{
				const float currentDepth = currentPixel.getUnorderedSample(currentSample, Chan_DeepFront);
				if ((currentDepth >= _deepCSpec.nearDOF) && (currentDepth <= _deepCSpec.farDOF))
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

	inline bool adjustSamples(DD::Image::Box box, const ChannelSet& channels, DeepOp* input0, const DeepOutputPlane& inPlane, DeepOutputPlane& outPlane)
	{
		DeepPlane currentBoxPlane;
		int currentBoxY = -1;

		for (Box::iterator it = box.begin(); it != box.end(); it++)
		{
			if (aborted())
			{
				return true;
			}

			//for every new row of the box get the deep plane of the new row
			if (it.y != currentBoxY)
			{
				currentBoxPlane = DeepPlane();
				if (!input0->deepEngine({ box.x(),it.y,box.r(),it.y + 1 }, channels, currentBoxPlane))
				{
					return true;
				}
				currentBoxY = it.y;
			}

			const DeepPixel inPixel = inPlane.getPixel(it);
			const std::size_t sampleCount = inPixel.getSampleCount();
			DeepOutPixel outPixel;

			float cumulativeTransparency = 1.0f;
			float cumulativeNewSamplesTransparency = 1.0f;

			for (std::size_t isample = sampleCount; isample-- > 0; )
			{
				//oldAlpha is used frequently so is being cached essentially
				const float oldAlpha = inPixel.getOrderedSample(isample, Chan_Alpha);

				float newAlpha = oldAlpha / cumulativeTransparency;
				if (newAlpha > 1.0f)
				{
					newAlpha = 1.0f;
					//newAlpha used to be > 1.0f but is capped to 1.0f
					//this means the RGB colours must be changed to ensure the un-premultiplied colour remains the same
					//newRGB * newAlpha = oldRGb * oldAlpha
					//newRGB = oldRGb * oldAlpha / newAlpha
					//as 1.0f / cumulativeTransparency is the blur factor, and the blur factor needs to oldAlpha / newAlpha
					//cumulativeTransparency needs to be newAlpha / oldAlpha
					//NOTE: this is not the cumulativeTransparency, we are just reusing the variable to meet the current requirement
					//      and checks later on we break on newAlpha == 1.0f, so the real cumulativeTransparency will not be used afterwards
					cumulativeTransparency = 1.0f / oldAlpha;
				}

				const float deepFront = inPixel.getOrderedSample(isample, Chan_DeepFront);
				//if sample in DOF
				if ((deepFront >= _deepCSpec.nearDOF) && (deepFront <= _deepCSpec.farDOF))
				{
					//the sample should not be blurred, this is the same as saying it should contribute the same intensity to the final pixel
					//however if new samples have been added in front of it then it's light will be occluded, so needs to be adjusted to compensate this
					pushSample(inPixel, isample, channels, 1.0f / cumulativeNewSamplesTransparency, oldAlpha, outPixel);
					cumulativeTransparency -= oldAlpha;
					//this is not a new sample so cumulativeNewSamplesTransparency is not updated
					if ((cumulativeTransparency < 0.0f) || (newAlpha == 1.0f))
					{
						//if any new sample being added cannot be seen then there is no point adding it
						break;
					}
				}
				else
				{
					//the sample should be blurred, and have the same apparent intensity as it's current intensity without occlusion
					//therefore the sample data should be divided by the transparency to cancel out the loss of intensity via occlusion
					pushSample(inPixel, isample, channels, 1.0f / cumulativeTransparency, oldAlpha, newAlpha, outPixel);

					//keep track of the loss in transparency up to the current sample
					cumulativeTransparency -= oldAlpha;
					//keep track of the loss in transparency from samples in front of the DOF 
					cumulativeNewSamplesTransparency -= oldAlpha;

					//NOTE: if these conditions occur then there will be undefined behaviour for the program
					//      1 method of dealing with negative values is to not consider any pixels past this point as they cannot be seen anyway
					//      however have not tested to make sure that final colour value is exactly right when this occurs
					if ((cumulativeTransparency < 0.0f) || (newAlpha == 1.0f))
					{
						//if any new sample being added cannot be seen then there is no point adding it
						break;
					}
				}
			}

			outPlane.addPixel(outPixel);
		}
		return true;
	}

	bool doDeepEngine(DD::Image::Box box, const ChannelSet& channels, DeepOutputPlane& outPlane) override
	{
		DeepOp* input0 = dynamic_cast<DeepOp*>(input(0));
		if (!input0)
		{
			return true;
		}

#if 0
		DeepOutputPlane intermediateOutPlane = DeepOutputPlane(channels, box);
		if (!blurSamples(box, channels, input0, intermediateOutPlane))
		{
			return true;
		}

		outPlane = DeepOutputPlane(channels, box);
		if(!adjustSamples(box, channels, input0, intermediateOutPlane, outPlane))
		{
			return true;
		}
#else
		outPlane = DeepOutputPlane(channels, box);
		if (!blurSamples(box, channels, input0, outPlane))
		{
			return true;
		}
#endif
		
		return true;
	}

	static const Description d;
};


static Op* build(Node* node)
{
	return new DeepCDefocus(node);
}

const Op::Description DeepCDefocus::d(::CLASS, "Image/DeepCDefocus", build);