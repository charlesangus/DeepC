#include "DOFKernel.hpp"

DOFKernel::DOFKernel(const int blurRadius, const CircleMetadataKernel& metadataKernel) :
    blurRadius(blurRadius),
    dimension(2 * static_cast<std::size_t>(blurRadius) + 1)
{
    offsets.resize(dimension);
    pixelCount = metadataKernel.getPixelCount(blurRadius);
}

DOFKernel::~DOFKernel()
{
}
