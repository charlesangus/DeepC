#pragma once

#include <unordered_set>

#include "DeepCSpec.hpp"
#include "CircleMetadataKernel.hpp"

struct DOFKernelOffset
{
	int xOff;
	int yOff;
	bool isEdgePixel;
	
	DOFKernelOffset(int xOff, int yOff) : xOff(xOff), yOff(yOff), isEdgePixel(false) {}
	DOFKernelOffset(int xOff, int yOff, int isEdgePixel) : xOff(xOff), yOff(yOff), isEdgePixel(isEdgePixel) {}

	bool operator==(const DOFKernelOffset& other) const
	{
		return (xOff == other.xOff) && (yOff == other.yOff);
	}
};

namespace std
{
	//NOTE: this assumes that xOff and yOff are never more than half the size of an int
	//      as yOff if the moved to the upper half of the int to make a unique hash
	template<>
	struct hash<DOFKernelOffset>
	{
		std::size_t operator()(DOFKernelOffset const& kernelOffset) const noexcept
		{
			return kernelOffset.xOff | (kernelOffset.yOff << ((sizeof(int) / 2) * 8));
		}
	};
}

using DOFKernel = std::unordered_multiset<DOFKernelOffset>;

struct DeepCDefocusSpec : public DeepCSpec
{
	float maxBlurRadius;
	int maxBlurRadiusCeil;

	CircleMetadataKernel metadataKernel;
	std::vector<DOFKernel> kernels;

	float nearDOF;
	float nearDOFRate;
	float farDOF;
	float farDOFRate;

	float distribution_probability;

	DeepCDefocusSpec();
	~DeepCDefocusSpec();

	bool init(const float maxBlurSize);

	std::function<DOFKernel(const int blurRadius, const float distribution_percentage, const CircleMetadataKernel& metadataKernel)> getKernelFunction() const;
};
