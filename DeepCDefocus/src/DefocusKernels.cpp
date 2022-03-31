#include "DefocusKernels.hpp"

DOFIntensities getDOFIntensities(const float blurRadius, const CircleMetadataKernel& metadataKernel)
{
	DOFIntensities intensities;

	const float ceilBlurRadius = ceil(blurRadius);
	const float floorBlurRadius = ceilBlurRadius - 1.0f;

	const float ceilPixelCount = static_cast<float>(metadataKernel.getPixelCount(static_cast<int>(ceilBlurRadius)));
	const float floorPixelCount = static_cast<float>(metadataKernel.getPixelCount(static_cast<int>(blurRadius)));

	//linerally interpolate between floorPixelCount and ceilPixelCount, by how close the blurRadius is to the ceilBlurRadius
	//this allows for a smooth transition between kernels for any floating point blur radius
    //a + (b-a) * x
	//ceilPixelCount + (ceilPixelCount - floorPixelCount) * (1 - (ceilBlurRadius - blurRadius))
	intensities.innerPixelIntensity = 1.0f / (ceilPixelCount - (ceilPixelCount - floorPixelCount) * (ceilBlurRadius - blurRadius));

	//the intensity of edge pixels are decreased as only a percentage of these pixels should have the assigned 'intensity'
	//to compensate for this the 'itensity' is equally spread over the whole edge pixel
	intensities.edgePixelIntensity = intensities.innerPixelIntensity * (blurRadius - floorBlurRadius);

	return intensities;
}

CircleMetadataKernel getCircleMetadataKernel(int blurRadius)
{
	CircleMetadataKernel metadataKernel;
	metadataKernel.dimension = 2 * blurRadius + 1;
	metadataKernel.distances.resize(metadataKernel.dimension * metadataKernel.dimension);
	metadataKernel.pixelCounts.resize(blurRadius + 1, 0);

	for (int yOff = -blurRadius; yOff <= blurRadius; ++yOff)
	{
		for (int xOff = -blurRadius; xOff <= blurRadius; ++xOff)
		{
			const int x = xOff + blurRadius;
			const int y = yOff + blurRadius;
			int currentRadius = static_cast<int>(sqrtf(static_cast<float>(x * x + y * y)) + 0.5f);
			if (currentRadius <= blurRadius)
			{
				metadataKernel.distances[y * metadataKernel.dimension + x] = currentRadius;
				//this pixel will be apart of all kernels with an equal or greater radius
				for (int largerRadius = currentRadius; largerRadius <= blurRadius; ++largerRadius)
				{
					++metadataKernel.pixelCounts[largerRadius];
				}
			}
			else
			{
				metadataKernel.distances[y * metadataKernel.dimension + x] = INT_MAX;
			}
		}
	}

	return metadataKernel;
}

std::vector<float> getRawCircleKernel(const int blurRadius, const float, const CircleMetadataKernel& metadataKernel)
{
	const float intensity = 1.0f / metadataKernel.getPixelCount(blurRadius);

	const std::size_t kernelDimension = 2 * static_cast<std::size_t>(blurRadius) + 1;
	std::vector<float> kernel(kernelDimension * kernelDimension);
	for (int yOff = -blurRadius; yOff <= blurRadius; ++yOff)
	{
		for (int xOff = -blurRadius; xOff <= blurRadius; ++xOff)
		{
			const int x = xOff + blurRadius;
			const int y = yOff + blurRadius;
			if (static_cast<int>(sqrtf(static_cast<float>(x * x + y * y)) + 0.5f) <= blurRadius)
			{
				kernel[y * kernelDimension + x] = intensity;
			}
			else
			{
				kernel[y * kernelDimension + x] = 0.0f;
			}
		}
	}

	return kernel;
}

struct SeperableKernelData
{
	std::size_t kernelOff;
	std::vector<std::size_t> pixelCounts;
}

std::vector<float> getSeperableCircleKernels(const int blurRadius, const float, const CircleMetadataKernel& metadataKernel)
{
	const std::size_t pixelCount = metadataKernel.getPixelCount(blurRadius);
	const float intensity = 1.0f / static_cast<float>(pixelCount);
	
	const std::size_t kernelDimension = 2 * static_cast<std::size_t>(blurRadius) + 1;
	//the first kernelDimension floats represent the first stage seperable kernel (e.g. the X blur kernel)
	//the rest of the floats are all the second stage seperable kernels (e.g. Y blur kernels designed for each pixel output from the X blur)
	std::vector<float> sperableKernels(kernelDimension * kernelDimension + kernelDimension);
	
	for (int yOff = -blurRadius, y = 0; yOff <= blurRadius; ++yOff, ++y)
	{
		std::size_t columnPixelCount = 0;
		std::vector<float> pixelFlags(kernelDimension);
		for (int xOff = -blurRadius, x = 0; xOff <= blurRadius; ++xOff, ++x)
		{
			if (static_cast<int>(sqrtf(static_cast<float>(xOff * xOff + yOff * yOff)) + 0.5f) <= blurRadius)
			{
				++columnPixelCount;
				pixelFlags[x] = 1.0f;
			}
			else
			{
				pixelFlags[x] = 0.0f;
			}
		}
		const float columnPixelCountFloat = static_cast<float>(columnPixelCount);
		sperableKernels[y] = 1.0f / columnPixelCountFloat;
		for(int x = 0; x < kernelDimension; ++x)
		{
			sperableKernels[(y+1) * kernelDimension + x] = pixelFlags[x] / columnPixelCountFloat;
		}
	}
}

#if 0 
DOFKernel getCircleKernel(const int blurRadius, const float, const CircleMetadataKernel& metadataKernel)
{
	DOFKernel kernelData(blurRadius, metadataKernel);

	for (int y = -blurRadius; y <= blurRadius; ++y)
	{
		for (int x = -blurRadius; x <= blurRadius; ++x)
		{
			const int currentRadius = static_cast<int>(sqrtf(static_cast<float>(x * x + y * y)) + 0.5f);
			if (currentRadius <= blurRadius)
			{
				kernelData.offsets[static_cast<std::size_t>(y + blurRadius)].insert(DOFKernel::OffsetData{x, currentRadius == blurRadius });
			}
		}
	}

	return kernelData;
}

DOFKernel getSpiralKernel(const int blurRadius, const float distribution_percentage, const CircleMetadataKernel& metadataKernel)
{
	DOFKernel kernelData(blurRadius, metadataKernel);

	const float actualPixelAmountFloat = static_cast<float>(kernelData.pixelCount);
	const int approximatedPixelCount = static_cast<int>(kernelData.pixelCount * distribution_percentage);

	//implemention of spiral algorithm mathematically described in "Point Picking and Distributing on the Disc and Sphere" by Mary K Arthur
	//at page 23 of https://apps.dtic.mil/sti/pdfs/ADA626479.pdf [accessed 24/03/22]
	for (std::size_t pixel = 1; pixel <= approximatedPixelCount; ++pixel)
	{
		//polar coords of spiral sample
		const float pixelFloat = static_cast<float>(pixel);
		const float currentRadius = blurRadius * sqrtf(pixelFloat / actualPixelAmountFloat);
		const float currentAngle = 2.0f * sqrtf(M_PI_F * static_cast<float>(pixel));

		//round the sample coord to the nearest pixel coord
		const int xOff = static_cast<int>((currentRadius * cosf(currentAngle)) + 0.5f);
		const int yOff = static_cast<int>((currentRadius * sinf(currentAngle)) + 0.5f);

		//mapping the y offset from [-blurRadius,blurRadius] to [0,2*blurRadius+1]
		const std::size_t yOffIndex = static_cast<std::size_t>(yOff + kernelData.blurRadius);

		auto pixelItr = kernelData.offsets[yOffIndex].find(DOFKernel::OffsetData{ xOff });
		if (pixelItr == kernelData.offsets[yOffIndex].end())
		{
			//store the pixel in a row of spiral pixels, that all have the same y offsets
			kernelData.offsets[yOffIndex].insert(DOFKernel::OffsetData{ xOff, metadataKernel.isEdgePixel(xOff, yOff, kernelData.blurRadius)});
		}
		else
		{
			//allows for the same pixel to be in the kernel more than once, to ensure that the new pixelCount is correct
			++(pixelItr->intensityCount);
		}
	}

	return kernelData;
}
#endif