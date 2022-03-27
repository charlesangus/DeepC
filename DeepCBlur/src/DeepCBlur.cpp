#include <cstdio>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <vector>
#include <array>

#include "DeepCOp.hpp"
#include "OpTreeFactory.hpp"
#include "DeepCBlurSpec.hpp"
#include "BlurKernels.hpp"

using namespace DD::Image;

static const char* const CLASS = "DeepCBlur";

static const char* const BLUR_FILTER_TEXT = "filter";
static const char* const BLUR_QUALITY_TEXT = "quality";
static const char* const BLUR_MODE_TEXT = "mode";
static const char* const BLUR_SIZE_TEXT = "size";

static const char* const GAUSSIAN_BLUR_NAME = "gaussian";
static const char* const BLUR_TYPE_NAMES[] = { GAUSSIAN_BLUR_NAME, 0 };

static const char* const LOW_QUALITY_NAME = "low quality";
static const char* const MEDIUM_QUALITY_NAME = "medium quality";
static const char* const HIGH_QUALITY_NAME = "high quality";
static const char* const BLUR_QUALITY_NAMES[] = { LOW_QUALITY_NAME, MEDIUM_QUALITY_NAME, HIGH_QUALITY_NAME, 0 };

static const char* const RAW_GAUSSIAN_MODE_NAME = "raw gaussian";
static const char* const TRANSPARENT_MODIFIED_GAUSSIAN_MODE_NAME = "transparent modified gaussian";
static const char* const MODIFIED_GAUSSIAN = "modified gaussian";
static const char* const BLUR_MODE_NAMES[] = { RAW_GAUSSIAN_MODE_NAME,
											   TRANSPARENT_MODIFIED_GAUSSIAN_MODE_NAME,
											   MODIFIED_GAUSSIAN,
											   0 };

class DeepCBlur : public DeepCOp<DeepCBlurSpec>
{
private:
	
	float _blurSize;

	DeepCOpTree _opTree;
	bool _recomputeOpTree;

public:
	DeepCBlur(Node* node) : 
		DeepCOp<DeepCBlurSpec>(node),
		_blurSize(3.0f)
	{
#ifdef DEEPC_DEBUG
		printf("NOTE: DEEPC_DEBUG is enabled, to disable it comment out the define in DeepCDebugging.hpp\n");
#endif
		inputs(1);

		_deepCSpec.blurRadius = 4.5f; // 3.0 * 1.5
		_deepCSpec.blurRadiusFloor = 4;

		auto kernelFunction = _deepCSpec.getKernelFunction();
		if (kernelFunction == nullptr)
		{
			Op::error("failed to create DeepCBlur blur kernel");
		}
		else
		{
			_deepCSpec.kernel = kernelFunction(getSD(_blurSize), _deepCSpec.blurRadiusFloor);
			_deepCSpec.zKernel = kernelFunction(getSD(_blurSize), _deepCSpec.zKernelRadius);
			_deepCSpec.computeVolumetrics(_deepCSpec.blurRadius);
		}

		_recomputeOpTree = true;
	}

	~DeepCBlur(){}

	const char* node_help() const override
	{
		return "Perfoms a blur on the deep image, with the option to blur in depth in addition to the normal deep 2D blur";
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
		Divider(f, "Blur parameters");

		Enumeration_knob(f, &_deepCSpec.blurType, BLUR_TYPE_NAMES, BLUR_FILTER_TEXT, BLUR_FILTER_TEXT);
		Tooltip(f, "The type of blur kernel that should be used for blurring");

		Enumeration_knob(f, &_deepCSpec.blurQuality, BLUR_QUALITY_NAMES, BLUR_QUALITY_TEXT, BLUR_QUALITY_TEXT);
		Tooltip(f, "The quality/accuracy of the values in the blur kernel, a higher quality blur is more realisitc");

		Enumeration_knob(f, &_deepCSpec.blurMode, BLUR_MODE_NAMES, BLUR_MODE_TEXT, BLUR_MODE_TEXT);
		Tooltip(f, "The mode that should be used to calculate the RGBA values of the resulting image\n"
			       "'raw guassian' = gaussian blur of the RGB and A, results in a darkened image when composited\n"
			       "'transparent composited gaussian' = gaussian blur of pre-processed RGB, and sets A ~= 0, results in a normal RGB image with unrealistic alphas\n"
			       "'modified gaussain' = gaussain blur of pre-processed RGB and A, but post-processes all samples to make the resulting image look correct");

		Float_knob(f, &_blurSize, BLUR_SIZE_TEXT, BLUR_SIZE_TEXT);
		SetRange(f, 1.0f, 50.0f);
		Tooltip(f, "The size of the blur kernel\n"
                   "sets 'blur radius' = floor( 1.5 * 'blur size' )\n"
				   "sets 'standard deviation' = 'blur size' * 0.42466");
		
		advancedBlurParameters(f);
		//sampleOptimisationTab(f);
	}

	int knob_changed(DD::Image::Knob* k) override
	{
		int err = DeepCOp::knob_changed(k);
		if(err != 1)
		{
			return err;
		}
		_recomputeOpTree = true;
		return 1;
	}

	void _open() override
	{
#ifdef DEEPC_DEBUG
		printf("NOTE: DEEPC_DEBUG is enabled, to disable it comment out the define in DeepCDebugging.hpp\n");
#endif
		if (_recomputeOpTree)
		{
			_deepCSpec.blurRadius = 1.5f * _blurSize;
			_deepCSpec.blurRadiusFloor = static_cast<int>(_deepCSpec.blurRadius);
			
			auto kernelFunction = _deepCSpec.getKernelFunction();
			if (kernelFunction == nullptr)
			{
				Op::error("failed to create DeepCBlur blur kernel");
				return;
			}

			_deepCSpec.kernel = kernelFunction(getSD(_blurSize), _deepCSpec.blurRadiusFloor);
			_deepCSpec.zKernel = kernelFunction(getSD(_blurSize), _deepCSpec.zKernelRadius);

			_deepCSpec.computeVolumetrics(_deepCSpec.blurRadius);
			_recomputeOpTree = false;
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
		if (_recomputeOpTree)
		{
			_opTree = OpTreeFactory(_deepCSpec.doZBlur,_deepCSpec.blurMode, node(), _deepCSpec);
			if (!(_opTree.isValid() && _opTree.set_input(input(0)) && _opTree.set_parent(this)))
			{
				Op::error("failed to create DeepCBlur internal op tree");
				_deepInfo = DeepInfo();
				return;
			}
		}

		if ((_deepCSpec.blurRadiusFloor > _deepInfo.format()->width()) || (_deepCSpec.blurRadiusFloor > _deepInfo.format()->height()))
		{
			Op::error("blur radius is too large, it cannot be larger than the image");
			_deepInfo = DeepInfo();
			return;
		}

		if (!_opTree.validate(for_real))
		{
			Op::error("DeepCBlur internal op tree failed validate");
			_deepInfo = DeepInfo();
			return;
		}
		_deepInfo = _opTree.deepInfo();
	}

	void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests) override
	{
		DeepOp* lastOp = dynamic_cast<DeepOp*>(_opTree.back());
		if (lastOp != nullptr)
		{
			requests.push_back(RequestData(lastOp, box, channels, 1));
		}
	}

	bool doDeepEngine(DD::Image::Box box, const ChannelSet& channels, DeepOutputPlane& outPlane) override
	{
		if (!_opTree.isValid())
		{
			Op::error("DeepCBlur internal op tree is invalid");
			return false;
		}

		DeepPlane inPlane;
		if (!_opTree.deepEngine(box, channels, inPlane))
		{
			return true;
		}

		outPlane = DeepOutputPlane(channels, box);

		if (_deepCSpec.blurMode == DEEPC_MODIFIED_GAUSSIAN_MODE)
		{
			for (Box::iterator it = box.begin(); it != box.end(); it++)
			{
				const DeepPixel inPixel = inPlane.getPixel(it);
				const std::size_t sampleCount = inPixel.getSampleCount();
				DeepOutPixel outPixel;

				float cumulativeTransparency = 1.0f;
				for (std::size_t isample = sampleCount; isample-- > 0;)
				{
					float newAlpha = inPixel.getOrderedSample(isample, Chan_Alpha) / cumulativeTransparency;
					if (newAlpha > 1.0f)
					{
						newAlpha = 1.0f;
					}
					foreach(z, channels)
					{
						if ((z == Chan_Red) || (z == Chan_Green) || (z == Chan_Blue))
						{
							outPixel.push_back(inPixel.getOrderedSample(isample, z) / cumulativeTransparency);
						}
						else if (z == Chan_Alpha)
						{
							outPixel.push_back(newAlpha);
						}
						else
						{
							outPixel.push_back(inPixel.getOrderedSample(isample, z));
						}
					}

					//this in a sense brightens the colour of the sample
					//done to counteract the effect of the'over operation' reducing the sample's apparent intensity
					//increases the sample alpha to balance out the increase in RGB so that the un-premultiplied colour remains the same
					//compound the brightening effect for samples that have even more samples in front of them
					//the final result mimics the behaviour that the samples are now added together rather than being composited together
					//this is the desired beahviour as a flat gaussian adds up neighbours values without doing compoisiting 
					//T(n) = T(n-1) * ( 1 - ( alpha(n-1) / T(n-1) ) ) === T(n-1) - alpha(n-1) 
					//cumulativeTransparency *= (1.0f - (inPixel.getOrderedSample(isample, Chan_Alpha) / cumulativeTransparency));
					cumulativeTransparency -= inPixel.getOrderedSample(isample, Chan_Alpha);
					
					//NOTE: if targetPixelCumulativeTransparency < 0 that this will be undefined behaviour for the program
					//1 method of dealing with negative values is to not consider any pixels past this point as they cannot be seen anyway
					//however have not tested to make sure that final colour value is exactly right when this occurs
					if ((cumulativeTransparency < 0.0f) || (newAlpha == 1.0f))
					{
						break;
					}
				}

				outPlane.addPixel(outPixel);
			}
		}
		else
		{
			for (Box::iterator it = box.begin(); it != box.end(); it++)
			{
				const DeepPixel inPixel = inPlane.getPixel(it);
				const std::size_t sampleCount = inPixel.getSampleCount();
				DeepOutPixel outPixel;

				for (std::size_t isample = 0; isample < sampleCount; ++isample)
				{
					foreach(z, channels)
					{
						outPixel.push_back(inPixel.getUnorderedSample(isample, z));
					}
				}

				outPlane.addPixel(outPixel);
			}
		}



		return true;
	}

	static const Description d;
};


static Op* build(Node* node)
{
	return new DeepCBlur(node);
}

const Op::Description DeepCBlur::d(::CLASS, "Image/DeepCBlur", build);