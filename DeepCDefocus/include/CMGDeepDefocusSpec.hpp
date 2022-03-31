#pragma once

#include "CMGDeepSpec.hpp"
#include "CircleMetadataKernel.hpp"
#include "DOFKernel.hpp"

struct DeepCDefocusSpec : public CMGDeepSpec
{
	float maxBlurRadius;
	int maxBlurRadiusCeil;

	CircleMetadataKernel metadataKernel;
	std::vector<std::vector<float>> kernels;
	std::vector<DOFKernel> DOFKernels;

	float nearDOF;
	float farDOF;

	float distribution_probability;

	DeepCDefocusSpec();
	~DeepCDefocusSpec();

	std::function<std::vector<float>(const int blurRadius, const float distribution_percentage, const CircleMetadataKernel& metadataKernel)> getKernelFunction() const;
};