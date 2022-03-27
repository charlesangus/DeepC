#include "DeepCBlurSpec.hpp"

std::vector<float> getLQGaussianKernel(const float sd, const int blurRadius);
std::vector<float> getMQGaussianKernel(const float sd, const int blurRadius);
std::vector<float> getHQGaussianKernel(const float sd, const int blurRadius);

DeepCBlurSpec::DeepCBlurSpec()
{
	blurType = DEEPC_GAUSSIAN_BLUR;
	blurQuality = DEEPC_MEDIUM_QUALITY;
	blurMode = DEEPC_MODIFIED_GAUSSIAN_MODE;
}

DeepCBlurSpec::~DeepCBlurSpec()
{
}

std::function<std::vector<float>(const float sd, const int blurRadius)> DeepCBlurSpec::getKernelFunction() const
{
	switch (blurType)
	{
		case DEEPC_GAUSSIAN_BLUR:
			switch (blurQuality)
			{
				case DEEPC_LOW_QUALITY:    return getLQGaussianKernel;
				case DEEPC_MEDIUM_QUALITY: return getMQGaussianKernel;
				case DEEPC_HIGH_QUALITY:   return getHQGaussianKernel;
				default:                 return nullptr;
			}
		default:                		 return nullptr;
	}
}