// Kernel sums read straight off the node's disc kernel (DiscKernelLUT and the
// scatter's bracket blend), for the harness gates whose oracle is the kernel
// itself rather than the composite.
//
//   kernel_sums ramp size focus yHorizon yFocus softness height y0 y1
//
// A full-width plane receding in y with one point sample per pixel at depth
// z(y) = focus * (yHorizon - yFocus) / (yHorizon - y), defocused in manual
// mode (radius = size * |1 - focus / z|).  For each destination row y in
// [y0, y1) it prints
//
//   y S N dS
//
// S is the adjoint sum: the total kernel weight the source rows [0, height)
// deliver to one pixel of row y, i.e. sum over y' of row (y - y') of the
// kernel at radius(y'), summed across the row.  It is independent of x for a
// column far enough from the frame edges that every source reaching it
// exists.  N is how many nonzero taps make up that sum (one per source
// pixel whose kernel reaches the pixel), which is the term count of every
// per-pixel accumulator the deposits feed.  dS is how far S moves when every
// radius is scaled by (1 +/- 2^-21), i.e. by eight float ulps: the radii
// here are computed in float from the analytic depth, and that is the
// headroom the node's own float depth and CoC arithmetic is allowed.
//
//   kernel_sums taps softness r1 [r2 ...]
//
// For each radius: `r N W`, the nonzero tap count and weight sum of the
// kernel the scatter rasterises at that radius.  A full-frame layer at one
// radius reaches every pixel with exactly N nonzero terms.
//
// Unlike vref and thinlens_ref this includes src/ headers on purpose: what it
// reports is a property of the kernel the node ships, so it must read that
// kernel, not a re-derivation of it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "DeepCDefocusKernel.h"
#include "DeepCDefocusScatter.h"

namespace {

struct KernelRows {
    int radiusY = 0;
    std::vector<double> mass;
    std::vector<long> taps;
};

KernelRows rasterise(const deepc::DiscKernelLUT& lut, float radius)
{
    KernelRows out;
    if (!(radius > deepc::kSharpRadiusPx)) {
        out.mass.assign(1, 1.0);
        out.taps.assign(1, 1);
        return out;
    }
    const deepc::KernelGridBracket br = deepc::kernelGridBracket(radius);
    const bool blend = (br.indexB != br.indexA) && (br.frac != 0.0f);
    const deepc::KernelView kvA = lut.kernel(deepc::kernelGridRadius(br.indexA), 0, 0, 0.0f, 0);
    deepc::KernelView kvB;
    if (blend)
        kvB = lut.kernel(deepc::kernelGridRadius(br.indexB), 0, 0, 0.0f, 0);
    const int rx = std::max(kvA.radiusX, blend ? kvB.radiusX : 0);
    const int ry = std::max(kvA.radiusY, blend ? kvB.radiusY : 0);
    out.radiusY = ry;
    out.mass.assign(static_cast<std::size_t>(2 * ry + 1), 0.0);
    out.taps.assign(static_cast<std::size_t>(2 * ry + 1), 0);
    std::vector<float> row(static_cast<std::size_t>(2 * rx + 1));
    for (int dy = -ry; dy <= ry; ++dy) {
        const float* w = nullptr;
        int count = 0;
        if (blend) {
            int xs = 0;
            count = deepc::blendBracketRow(kvA, 1.0f - br.frac, kvB, br.frac, dy, row.data(), xs);
            w = row.data();
        } else {
            const int r = dy + kvA.radiusY;
            if (r < 0 || r >= kvA.rowCount || kvA.row(r).empty())
                continue;
            count = kvA.row(r).count();
            w = kvA.rowWeights(r);
        }
        double m = 0.0;
        long n = 0;
        for (int k = 0; k < count; ++k) {
            m += static_cast<double>(w[k]);
            n += (w[k] != 0.0f) ? 1 : 0;
        }
        out.mass[static_cast<std::size_t>(dy + ry)] = m;
        out.taps[static_cast<std::size_t>(dy + ry)] = n;
    }
    return out;
}

int usage()
{
    std::fprintf(stderr,
                 "usage: kernel_sums ramp size focus yHorizon yFocus softness height y0 y1\n"
                 "       kernel_sums taps softness r1 [r2 ...]\n");
    return 2;
}

int ramp(int argc, char** argv)
{
    if (argc != 10)
        return usage();
    const float  size     = std::strtof(argv[2], nullptr);
    const float  focus    = std::strtof(argv[3], nullptr);
    const double yHorizon = std::strtod(argv[4], nullptr);
    const double yFocus   = std::strtod(argv[5], nullptr);
    const float  softness = std::strtof(argv[6], nullptr);
    const int    height   = std::atoi(argv[7]);
    const int    y0       = std::atoi(argv[8]);
    const int    y1       = std::atoi(argv[9]);
    const double c        = static_cast<double>(focus) * (yHorizon - yFocus);

    std::vector<float> radius(static_cast<std::size_t>(height));
    float rMax = 0.0f;
    for (int y = 0; y < height; ++y) {
        const float invZ = static_cast<float>((yHorizon - y) / c);
        const float z = 1.0f / invZ;
        radius[static_cast<std::size_t>(y)] = size * std::fabs(1.0f - focus * (1.0f / z));
        rMax = std::max(rMax, radius[static_cast<std::size_t>(y)]);
    }
    const float nudge = std::ldexp(1.0f, -21);
    const deepc::DiscKernelLUT lut(0.0f, rMax * (1.0f + 2.0f * nudge) + 1.0f, softness, 1.0f);

    std::vector<double> sum[3];
    std::vector<long> taps(static_cast<std::size_t>(y1 - y0), 0);
    const float scale[3] = { 1.0f, 1.0f + nudge, 1.0f - nudge };
    for (int v = 0; v < 3; ++v) {
        sum[v].assign(static_cast<std::size_t>(y1 - y0), 0.0);
        for (int ys = 0; ys < height; ++ys) {
            const KernelRows k = rasterise(lut, radius[static_cast<std::size_t>(ys)] * scale[v]);
            for (int dy = -k.radiusY; dy <= k.radiusY; ++dy) {
                const int y = ys + dy;
                if (y < y0 || y >= y1)
                    continue;
                const std::size_t i = static_cast<std::size_t>(y - y0);
                const std::size_t r = static_cast<std::size_t>(dy + k.radiusY);
                sum[v][i] += k.mass[r];
                if (v == 0)
                    taps[i] += k.taps[r];
            }
        }
    }
    std::printf("# ramp size %.9g focus %.9g yHorizon %.17g yFocus %.17g softness %.9g "
                "height %d rows [%d, %d)\n",
                size, focus, yHorizon, yFocus, softness, height, y0, y1);
    for (int y = y0; y < y1; ++y) {
        const std::size_t i = static_cast<std::size_t>(y - y0);
        const double dS = std::max(std::fabs(sum[1][i] - sum[0][i]),
                                   std::fabs(sum[2][i] - sum[0][i]));
        std::printf("%d %.17g %ld %.17g\n", y, sum[0][i], taps[i], dS);
    }
    return 0;
}

int taps(int argc, char** argv)
{
    if (argc < 4)
        return usage();
    const float softness = std::strtof(argv[2], nullptr);
    std::vector<float> radii;
    float rMax = 0.0f;
    for (int i = 3; i < argc; ++i) {
        radii.push_back(std::strtof(argv[i], nullptr));
        rMax = std::max(rMax, radii.back());
    }
    const deepc::DiscKernelLUT lut(0.0f, rMax + 1.0f, softness, 1.0f);
    for (const float r : radii) {
        const KernelRows k = rasterise(lut, r);
        long n = 0;
        double w = 0.0;
        for (std::size_t i = 0; i < k.mass.size(); ++i) {
            n += k.taps[i];
            w += k.mass[i];
        }
        std::printf("%.9g %ld %.17g\n", r, n, w);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
        return usage();
    if (std::strcmp(argv[1], "ramp") == 0)
        return ramp(argc, argv);
    if (std::strcmp(argv[1], "taps") == 0)
        return taps(argc, argv);
    return usage();
}
