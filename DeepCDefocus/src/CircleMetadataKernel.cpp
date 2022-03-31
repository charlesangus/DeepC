#include "CircleMetadataKernel.hpp"

int CircleMetadataKernel::getDistance(int xOff, int yOff) const
{
	return distances[(yOff + blurRadius) * dimension + (xOff + blurRadius)];
}

std::size_t CircleMetadataKernel::getPixelCount(int blurRadius) const
{
	return pixelCounts[blurRadius];
}

std::size_t CircleMetadataKernel::getEdgePixelAmount(int blurRadius) const
{
	return pixelCounts[blurRadius] - pixelCounts[blurRadius-1];
}

bool CircleMetadataKernel::isEdgePixel(int xOff, int yOff, int blurRadius) const
{
	return getDistance(xOff, yOff) == blurRadius;
}
