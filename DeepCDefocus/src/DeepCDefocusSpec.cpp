#include "DeepCDefocusSpec.hpp"
#include "DefocusKernels.hpp"

DeepCDefocusSpec::DeepCDefocusSpec() : DeepCSpec()
{
	blurType = DEEPC_CIRCLE;
	blurQuality = DEEPC_FULL_CIRCLE;
	blurMode = DEEPC_DEFOCUS_MODE_UNKNOWN;

	nearDOF = 0.0f;
	nearDOFRate = 1.0f;
	farDOF = 1.0f;
	farDOFRate = 1.0f;

	distribution_probability = 0.5f;
}

DeepCDefocusSpec::~DeepCDefocusSpec()
{
}

bool DeepCDefocusSpec::init(const float maxBlurSize)
{
	maxBlurRadius = maxBlurSize * 1.5f;
	maxBlurRadiusCeil = static_cast<int>(ceil(maxBlurRadius));

	metadataKernel = std::move(getCircleMetadataKernel(maxBlurRadiusCeil));

	auto kernelFunction = getKernelFunction();
	if (kernelFunction == nullptr)
	{
		return false;
	}

	kernels.resize(maxBlurRadiusCeil);
	for (int radius = 1; radius <= maxBlurRadiusCeil; ++radius)
	{
		kernels[radius - 1] = std::move(kernelFunction(radius, distribution_probability, metadataKernel));
	}

	return true;
}

std::function<DOFKernel(const int blurRadius, const float distribution_percentage, const CircleMetadataKernel& metadataKernel)> DeepCDefocusSpec::getKernelFunction() const
{
	switch (blurType)
	{
		case DEEPC_CIRCLE:
			switch (blurQuality)
			{
				case DEEPC_FULL_CIRCLE:         return getCircleKernel;
				case DEEPC_APPROXIMATED_CIRCLE: return getSpiralKernel;
				default:                      return nullptr;
			}
		default:                              return nullptr;
	}
}