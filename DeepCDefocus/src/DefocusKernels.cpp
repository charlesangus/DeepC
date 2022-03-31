#include "DefocusKernels.hpp"

#include <algorithm>

CircleMetadataKernel getCircleMetadataKernel(int blurRadius)
{
	CircleMetadataKernel metadataKernel;
	metadataKernel.blurRadius = blurRadius;
	metadataKernel.dimension = 2 * blurRadius + 1;
	metadataKernel.distances.resize(metadataKernel.dimension * metadataKernel.dimension);
	metadataKernel.pixelCounts.resize(blurRadius + 1, 0);

	for (int yOff = -blurRadius, y = 0; yOff <= blurRadius; ++yOff, ++y)
	{
		for (int xOff = -blurRadius, x = 0; xOff <= blurRadius; ++xOff, ++x)
		{
			int currentRadius = static_cast<int>(sqrtf(static_cast<float>(xOff * xOff + yOff * yOff)) + 0.5f);
			if (currentRadius <= blurRadius)
			{
				//this pixel is an edge pixel of a kernel with currentRadius and approximately currentRadius from the center
				metadataKernel.distances[y * metadataKernel.dimension + x] = currentRadius;
				//this pixel will be apart of all kernels with an equal or greater radius
				for (int largerRadius = currentRadius; largerRadius <= blurRadius; ++largerRadius)
				{
					++metadataKernel.pixelCounts[largerRadius];
				}
			}
			else
			{
				//pixels outisde the largest kernel are given a default value to exclude them from all future operations
				metadataKernel.distances[y * metadataKernel.dimension + x] = INT_MAX;
			}
		}
	}

	return metadataKernel;
}

DOFKernel getCircleKernel(const int blurRadius, const float, const CircleMetadataKernel& metadataKernel)
{
	DOFKernel kernelData;

	for (int yOff = -blurRadius; yOff <= blurRadius; ++yOff)
	{
		for (int xOff = -blurRadius; xOff <= blurRadius; ++xOff)
		{
			const int currentRadius = static_cast<int>(sqrtf(static_cast<float>(xOff * xOff + yOff * yOff)) + 0.5f);
			if (currentRadius <= blurRadius)
			{
				kernelData.kernel.emplace(xOff,yOff);
			}
		}
	}

	kernelData.intensity = 1.0f / metadataKernel.getPixelCount(blurRadius);

	return kernelData;
}

DOFKernel getSpiralKernel(const int blurRadius, const float distribution_percentage, const CircleMetadataKernel& metadataKernel)
{
	DOFKernel kernelData;

	const std::size_t pixelCount = metadataKernel.getPixelCount(blurRadius);
	const float actualPixelAmountFloat = static_cast<float>(pixelCount);
	const int approximatedPixelCount = static_cast<int>(pixelCount * distribution_percentage);

	//implemention of spiral algorithm mathematically described in "Point Picking and Distributing on the Disc and Sphere" by Mary K Arthur
	//at page 23 of https://apps.dtic.mil/sti/pdfs/ADA626479.pdf [accessed 24/03/22]
	for (std::size_t pixel = 1; pixel <= approximatedPixelCount; ++pixel)
	{
		//polar coords of spiral sample
		const float currentRadius = blurRadius * sqrtf(static_cast<float>(pixel) / actualPixelAmountFloat);
		const float currentAngle = 2.0f * sqrtf(M_PI_F * static_cast<float>(pixel));

		//round the sample coord to the nearest pixel coord
		const int xOff = static_cast<int>((currentRadius * cosf(currentAngle)) + 0.5f);
		const int yOff = static_cast<int>((currentRadius * sinf(currentAngle)) + 0.5f);

		kernelData.kernel.emplace(xOff, yOff);
	}

	kernelData.intensity = 1.0f / static_cast<float>(approximatedPixelCount);

	return kernelData;
}

#if 0
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

//the intensity and then an inline vector of rows, a row consists of 
// 1) the amount of x offsets in the row
// 2) the y offset of the row
// 3) a sequence of x offsets 
//[ rowCount , yOff1, xOff1, xOff2, xOff3, ... , rowCount , yOff2, xOffa, xOffb, xOffc, ... ]
inline std::vector<float> compressKernel(const float intensity, const int blurRadius, const std::vector<std::size_t>& rowXCounts, const std::vector<std::vector<float>>& intermediateKernel)
{
	std::vector<float> kernel;
	kernel.reserve(1 + (kernelDimension * 2) + std::accumulate(rowXCounts.begin(), rowXCounts.end(), 0));
	kernel.push_back(intensity);
	for (int yOff = -blurRadius, y = 0; yOff <= blurRadius; ++yOff, ++y)
	{
		//1)
		kernel.push_back(rowXCounts[y]);
		//2)
		kernel.push_back(yOff);
		//3)
		for (int x = 0; x < rowXCounts[y]; ++x)
		{
			kernel.push_back(intermediateKernel[y][x]);
		}
	}
	return kernel;
}


std::vector<float> getRawCircleKernel(const int blurRadius, const float, const CircleMetadataKernel& metadataKernel)
{
	const std::size_t kernelDimension = 2 * static_cast<std::size_t>(blurRadius) + 1;
	std::vector<std::size_t> rowXCounts(kernelDimension,0);
	std::vector<std::vector<float>> intermediateKernel(kernelDimension);
	for (int yOff = -blurRadius, y=0; yOff <= blurRadius; ++yOff, ++y)
	{
		for (int xOff = -blurRadius; xOff <= blurRadius; ++xOff)
		{
			if (static_cast<int>(sqrtf(static_cast<float>(xOff * xOff + yOff * yOff)) + 0.5f) <= blurRadius)
			{
				intermediateKernel[y].push_back(xOff);
				++rowXCounts[y];
			}
		}
	}

	return compressKernel(1.0f / metadataKernel.getPixelCount(blurRadius), blurRadius, rowXCounts, intermediateKernel);
}

std::vector<float> getSpiralKernel(const int blurRadius, const float distribution_percentage, const CircleMetadataKernel& metadataKernel)
{
	const std::size_t kernelDimension = 2 * static_cast<std::size_t>(blurRadius) + 1;

	std::vector<std::size_t> rowXCounts(kernelDimension,0);
	std::vector<std::vector<float>> intermediateKernel(kernelDimension);

	const float actualPixelAmountFloat = static_cast<float>(kernelData.pixelCount);
	//to ensure that this is the final pixel count, the same pixel may be added to the spiral multiple times
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
		//may result in the same xOff and yOff for certain pixels if the sample coords are close
		const int xOff = static_cast<int>((currentRadius * cosf(currentAngle)) + 0.5f);
		const int yOff = static_cast<int>((currentRadius * sinf(currentAngle)) + 0.5f);

		//mapping the y offset from [-blurRadius,blurRadius] to [0,2*blurRadius+1]
		const std::size_t y = static_cast<std::size_t>(yOff + kernelData.blurRadius);

		intermediateKernel[y].push_back(xOff);
		++rowXCounts[y];
	}

	//sort the x offsets so that we iterate sequentially through the deep planes
	for (std::size_t y = 0; y < kernelDimension; ++y)
	{
		std::sort(intermediateKernel[y].begin(), intermediateKernel[y].end())
	}
	
	return compressKernel(1.0f / metadataKernel.getPixelCount(blurRadius), blurRadius, rowXCounts, intermediateKernel);

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

#endif