// Thin-lens reference for a volumetric card: an axis-aligned box of uniform
// optical density between zF and zB, optionally over an opaque card at bz.
//
//   vref x0 y0 x1 y1 zF zB alpha tintR tintG tintB size focus
//        bgOn bx0 by0 bx1 by1 bz bgR bgG bgB
//        wx0 wy0 wx1 wy1 nSub nLens reps seed
//
// The box is [x0,x1) x [y0,y1) in pixels and alpha is its opacity through
// the full thickness; tint is its unpremultiplied colour.  bgOn = 1 adds an
// opaque card [bx0,bx1) x [by0,by1) at depth bz with colour bg (the bg*
// arguments are still required, and ignored, when bgOn = 0).  size and focus
// are the node's manual-mode lens: signed CoC c(z) = size * (1 - focus / z),
// |c| the blur radius in pixels.  Every pixel of the window [wx0,wx1) x
// [wy0,wy1) is integrated over nSub x nSub pixel strata times nLens x nLens
// lens strata (concentric map, so the strata stay equal-area), each jittered,
// repeated reps times with independent jitter; the pixel is the mean over the
// replicates and its standard error is sd(replicate means) / sqrt(reps).
// seed fixes the jitter, and each pixel draws from its own stream, so the
// output does not depend on the thread count.
//
// Output, one line per pixel:  x y R G B A seR seG seB seA tau seTau
// (premultiplied colour, "%.9g"), after one '#' line echoing the arguments.
// tau is the mean optical depth through the box, E[sigma * path length]: the
// alpha a composite would read if it treated every slice of the volume as
// occupying disjoint area, i.e. with no nesting of slices in lens space.
//
// Build: any C++17 compiler; -fopenmp is optional.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kChannels = 5;

struct Rng
{
    uint64_t state;

    double next()
    {
        uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
    }
};

// A pixel's stream starts from a hashed state rather than seed + index, so
// neighbouring pixels do not draw shifted copies of one sequence.
uint64_t mix(uint64_t h, uint64_t v)
{
    Rng r{ h ^ (v * 0xd6e8feb86659fd93ull) };
    r.next();
    return r.state ^ static_cast<uint64_t>(r.next() * 9007199254740992.0);
}

void concentric(double a, double b, double& ux, double& uy)
{
    a = 2.0 * a - 1.0;
    b = 2.0 * b - 1.0;
    if (a == 0.0 && b == 0.0) {
        ux = uy = 0.0;
        return;
    }
    double r, phi;
    if (std::fabs(a) > std::fabs(b)) {
        r = a;
        phi = kPi / 4.0 * (b / a);
    } else {
        r = b;
        phi = kPi / 2.0 - kPi / 4.0 * (a / b);
    }
    ux = r * std::cos(phi);
    uy = r * std::sin(phi);
}

struct Lens
{
    double size;
    double focus;

    double coc(double z) const { return size * (1.0 - focus / z); }
};

// A ray's image position at depth z is q + c(z) u, and c is affine in
// w = 1/z, so the set of w where the ray is inside [lo, hi) on one axis is an
// interval: that is what makes the path length through the box exact.
void clipAxis(double q, double u, const Lens& lens, double lo, double hi,
              double& wLo, double& wHi)
{
    const double a = q + lens.size * u;
    const double b = -lens.size * lens.focus * u;
    if (std::fabs(b) < 1e-15) {
        if (!(a >= lo && a < hi)) {
            wLo = 1.0;
            wHi = 0.0;
        }
        return;
    }
    double w0 = (lo - a) / b;
    double w1 = (hi - a) / b;
    if (w0 > w1)
        std::swap(w0, w1);
    if (w0 > wLo)
        wLo = w0;
    if (w1 < wHi)
        wHi = w1;
}

struct Scene
{
    double box[4];
    double zF, zB, sigma;
    double tint[3];
    bool bgOn;
    double bgBox[4];
    double bgZ;
    double bg[3];
};

void traceRay(const Scene& s, const Lens& lens, double qx, double qy,
              double ux, double uy, double out[5])
{
    double wLo = 1.0 / s.zB;
    double wHi = 1.0 / s.zF;
    clipAxis(qx, ux, lens, s.box[0], s.box[2], wLo, wHi);
    clipAxis(qy, uy, lens, s.box[1], s.box[3], wLo, wHi);
    const double tau = wHi > wLo ? s.sigma * (1.0 / wLo - 1.0 / wHi) : 0.0;
    const double t = std::exp(-tau);
    const double a = 1.0 - t;
    out[0] = s.tint[0] * a;
    out[1] = s.tint[1] * a;
    out[2] = s.tint[2] * a;
    out[3] = a;
    out[4] = tau;
    if (s.bgOn) {
        const double c = lens.coc(s.bgZ);
        const double bx = qx + c * ux;
        const double by = qy + c * uy;
        if (bx >= s.bgBox[0] && bx < s.bgBox[2] && by >= s.bgBox[1] && by < s.bgBox[3]) {
            for (int i = 0; i < 3; ++i)
                out[i] += t * s.bg[i];
            out[3] += t;
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 30) {
        std::fprintf(stderr,
                     "usage: vref x0 y0 x1 y1 zF zB alpha tintR tintG tintB size focus "
                     "bgOn bx0 by0 bx1 by1 bz bgR bgG bgB wx0 wy0 wx1 wy1 nSub nLens reps seed\n");
        return 2;
    }
    int k = 1;
    auto num = [&]() { return std::atof(argv[k++]); };
    auto whole = [&]() { return std::atoi(argv[k++]); };

    Scene s;
    for (double& v : s.box)
        v = num();
    s.zF = num();
    s.zB = num();
    const double alpha = num();
    for (double& v : s.tint)
        v = num();
    Lens lens;
    lens.size = num();
    lens.focus = num();
    s.bgOn = whole() != 0;
    for (double& v : s.bgBox)
        v = num();
    s.bgZ = num();
    for (double& v : s.bg)
        v = num();
    const int wx0 = whole(), wy0 = whole(), wx1 = whole(), wy1 = whole();
    const int nSub = whole(), nLens = whole(), reps = whole();
    const uint64_t seed = std::strtoull(argv[k++], nullptr, 10);

    if (!(s.zB > s.zF && s.zF > 0.0 && alpha >= 0.0 && alpha < 1.0)
        || wx1 <= wx0 || wy1 <= wy0 || nSub < 1 || nLens < 1 || reps < 2) {
        std::fprintf(stderr, "vref: need 0 < zF < zB, 0 <= alpha < 1, a non-empty window, "
                             "nSub, nLens >= 1 and reps >= 2\n");
        return 2;
    }
    s.sigma = -std::log(1.0 - alpha) / (s.zB - s.zF);

    const int width = wx1 - wx0;
    const int height = wy1 - wy0;
    std::vector<double> result(static_cast<size_t>(width) * height * 2 * kChannels, 0.0);

#pragma omp parallel for schedule(dynamic, 1)
    for (int py = wy0; py < wy1; ++py) {
        std::vector<double> means(static_cast<size_t>(reps) * kChannels);
        for (int px = wx0; px < wx1; ++px) {
            const uint64_t pixelSeed = mix(mix(seed, static_cast<uint64_t>(px) + 0x10000ull),
                                           static_cast<uint64_t>(py) + 0x10000ull);
            for (int rep = 0; rep < reps; ++rep) {
                Rng rng{ mix(pixelSeed, static_cast<uint64_t>(rep)) };
                double acc[kChannels] = {};
                for (int sy = 0; sy < nSub; ++sy)
                for (int sx = 0; sx < nSub; ++sx) {
                    for (int ly = 0; ly < nLens; ++ly)
                    for (int lx = 0; lx < nLens; ++lx) {
                        const double qx = px + (sx + rng.next()) / nSub;
                        const double qy = py + (sy + rng.next()) / nSub;
                        double ux, uy;
                        concentric((lx + rng.next()) / nLens, (ly + rng.next()) / nLens, ux, uy);
                        double ray[kChannels];
                        traceRay(s, lens, qx, qy, ux, uy, ray);
                        for (int i = 0; i < kChannels; ++i)
                            acc[i] += ray[i];
                    }
                }
                const double n = static_cast<double>(nSub) * nSub * nLens * nLens;
                for (int i = 0; i < kChannels; ++i)
                    means[static_cast<size_t>(rep) * kChannels + i] = acc[i] / n;
            }
            double* o = &result[(static_cast<size_t>(py - wy0) * width + (px - wx0)) * 2 * kChannels];
            for (int i = 0; i < kChannels; ++i) {
                double mean = 0.0;
                for (int rep = 0; rep < reps; ++rep)
                    mean += means[static_cast<size_t>(rep) * kChannels + i];
                mean /= reps;
                double var = 0.0;
                for (int rep = 0; rep < reps; ++rep) {
                    const double d = means[static_cast<size_t>(rep) * kChannels + i] - mean;
                    var += d * d;
                }
                var /= (reps - 1);
                o[i] = mean;
                o[kChannels + i] = std::sqrt(var / reps);
            }
        }
    }

    std::printf("#");
    for (int i = 1; i < argc; ++i)
        std::printf(" %s", argv[i]);
    std::printf("\n");
    for (int py = wy0; py < wy1; ++py)
        for (int px = wx0; px < wx1; ++px) {
            const double* o = &result[(static_cast<size_t>(py - wy0) * width + (px - wx0)) * 2 * kChannels];
            std::printf("%d %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", px, py,
                        o[0], o[1], o[2], o[3], o[5], o[6], o[7], o[8], o[4], o[9]);
        }
    return 0;
}
