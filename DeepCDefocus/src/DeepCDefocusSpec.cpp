#include "DeepCDefocusSpec.hpp"

#include "DefocusKernels.hpp"

DeepCDefocusSpec::DeepCDefocusSpec() : CMGDeepSpec()
{
	blurType = CMG_CIRCLE;
	blurQuality = CMG_FULL_CIRCLE;
	blurMode = CMG_DEEP_DEFOCUS_BLUR_MODE_UNKNOWN;
}

DeepCDefocusSpec::~DeepCDefocusSpec()
{
}

std::function<std::vector<float>(const int blurRadius, const float distribution_percentage, const CircleMetadataKernel& metadataKernel)> DeepCDefocusSpec::getKernelFunction() const
{
	switch (blurType)
	{
		case CMG_CIRCLE:
			return getRawCircleKernel;
			/*switch (blurQuality)
			{
				case CMG_FULL_CIRCLE:         return getCircleKernel;
				case CMG_APPROXIMATED_CIRCLE: return getSpiralKernel;
				default:                      return nullptr;
			}*/
		default:                              return nullptr;
	}
}