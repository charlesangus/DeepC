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
// Kernel mode:  vref kernel dMin dMax layers mergeTol <the arguments above>
//
// bounds, per pixel, what the node's disc kernel alone can move a composite
// of the box's pieces by.  The box is cut the way the node documents
// (uniform in CoC on each side of focus, focus a cut of its own, step
// max(2 max(mergeTol, 1/8), CoC variation over [dMin, dMax] / layers), at
// most layers + 1 pieces), each piece drawn at the radius of its depth
// midpoint with alpha 1 - (1 - alpha)^(thickness fraction), the opaque card
// one more layer at alpha 1.  For layer i, c_spec is the coverage the
// kernel SPEC gives the pixel (a disc point-sampled at integer offsets with a
// 1 px linear edge ramp, normalised, a delta at r <= 0.5, and radii off the
// kernel grid blended between the two bracketing grid radii) and c_true the
// exact coverage of that slice, integrated analytically over pixel, lens and
// depth.  Since |prod(1 - a_i x_i) - prod(1 - a_i y_i)| <= sum a_i |x_i - y_i|
// for x, y in [0, 1], K = sum_i a_i |c_spec,i - c_true,i| bounds the product
// composite of the kernel's coverages against the one of the exact ones.
// The sampling arguments are accepted and ignored: nothing here is sampled.
//
// The node does this arithmetic in float32, which the spec does not: a
// layer's coverage there is a sum of n_i(p) rounded weights times a value,
// each term rounded once for the weight and once for the product-accumulate,
// so it is within 2 n_i(p) u a_i of the spec's (u = 2^-24, n_i(p) the taps
// the node rasterises into the pixel for that layer, both bracketing entries
// counted), and compositing L layers rounds at most twice per layer.  Kfloat
// = u (2 sum_i a_i n_i(p) + 2 L) is that term, in the same units as K.
//
// Output: '#' lines echoing the arguments and listing the pieces (with the
// smallest radius gap between neighbouring pieces on one side of focus,
// which the node's pre-merge would fuse if it were within mergeTol), then
// per pixel
//   x y K Kcard Kfloat
// where Kcard is the opaque card's share of K ("%.9g").
//
// Build: any C++17 compiler; -fopenmp is optional.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

constexpr double kEdgeSoftnessPx = 1.0;
constexpr double kSharpRadiusPx = 0.5;
constexpr double kUnitRoundoff = 1.0 / 16777216.0;

struct Piece
{
    double zFront, zBack, alpha, radius;
};

double pieceStepPx(const Lens& lens, double dMin, double dMax, int layers, double mergeTol)
{
    const double rNear = lens.coc(dMin);
    const double rFar = lens.coc(dMax);
    const double variation = ((rNear < 0.0) == (rFar < 0.0))
                           ? std::fabs(rFar - rNear)
                           : std::fabs(rNear) + std::fabs(rFar);
    return std::max(2.0 * std::max(mergeTol, 0.125), variation / layers);
}

std::vector<Piece> cutPieces(const Lens& lens, double zF, double zB, double alpha,
                             double step, int maxPieces)
{
    std::vector<Piece> out;
    const auto emit = [&](double z0, double z1) {
        const double t = (z1 - z0) / (zB - zF);
        out.push_back({ z0, z1, 1.0 - std::pow(1.0 - alpha, t),
                        std::fabs(lens.coc(0.5 * (z0 + z1))) });
    };
    if (std::fabs(lens.coc(zF)) <= kSharpRadiusPx && std::fabs(lens.coc(zB)) <= kSharpRadiusPx) {
        emit(zF, zB);
        return out;
    }
    std::vector<double> ends = { zF };
    if (lens.focus > zF && lens.focus < zB && maxPieces >= 2)
        ends.push_back(lens.focus);
    ends.push_back(zB);
    const int sides = static_cast<int>(ends.size()) - 1;
    double partFront = zF;
    for (int side = 0; side < sides; ++side) {
        const double z0 = ends[side];
        const double z1 = ends[side + 1];
        const bool front = z1 <= lens.focus;
        const double r0 = std::fabs(lens.coc(z0));
        const double r1 = std::fabs(lens.coc(z1));
        const int room = maxPieces - static_cast<int>(out.size()) - (sides - 1 - side);
        const int n = std::max(1, std::min(room, static_cast<int>(std::ceil(std::fabs(r1 - r0) / step))));
        for (int j = 1; j < n; ++j) {
            const double r = r0 + (r1 - r0) * j / n;
            const double z = lens.focus / (front ? 1.0 + r / lens.size : 1.0 - r / lens.size);
            if (z > partFront && z < z1) {
                emit(partFront, z);
                partFront = z;
            }
        }
        emit(partFront, z1);
        partFront = z1;
    }
    return out;
}

// The grid's nodes are 512 / n for integer n up to r = 16, then every 0.5 px.
void gridBracket(double r, double& rA, double& rB, double& frac)
{
    if (r < 16.0) {
        const double n = std::ceil(512.0 / r);
        rA = 512.0 / n;
        rB = 512.0 / (n - 1.0);
    } else {
        rA = 16.0 + 0.5 * std::floor((r - 16.0) / 0.5);
        rB = rA + 0.5;
    }
    frac = (r - rA) / (rB - rA);
}

// A table over kernel offsets [-reach, reach]^2 held as its summed-area
// table, so its sum over any box of sources is four lookups.
struct OffsetTable
{
    int reach = 0;
    std::vector<double> sat;

    void build(int reachIn, const std::vector<double>& dense)
    {
        reach = reachIn;
        const int w = 2 * reach + 1;
        sat.assign(static_cast<size_t>(w + 1) * (w + 1), 0.0);
        for (int y = 0; y < w; ++y)
            for (int x = 0; x < w; ++x)
                sat[static_cast<size_t>(y + 1) * (w + 1) + (x + 1)] =
                    dense[static_cast<size_t>(y) * w + x]
                    + sat[static_cast<size_t>(y) * (w + 1) + (x + 1)]
                    + sat[static_cast<size_t>(y + 1) * (w + 1) + x]
                    - sat[static_cast<size_t>(y) * (w + 1) + x];
    }

    double at(int x, int y) const
    {
        const int w = 2 * reach + 2;
        return sat[static_cast<size_t>(y + reach + 1) * w + (x + reach + 1)];
    }

    // The sum over the sources in box that reach pixel (px, py).
    double overBox(int px, int py, const double box[4]) const
    {
        const int dx0 = std::max(px - static_cast<int>(std::ceil(box[2])) + 1, -reach);
        const int dy0 = std::max(py - static_cast<int>(std::ceil(box[3])) + 1, -reach);
        const int dx1 = std::min(px - static_cast<int>(std::ceil(box[0])), reach);
        const int dy1 = std::min(py - static_cast<int>(std::ceil(box[1])), reach);
        if (dx0 > dx1 || dy0 > dy1)
            return 0.0;
        return at(dx1, dy1) - at(dx0 - 1, dy1) - at(dx1, dy0 - 1) + at(dx0 - 1, dy0 - 1);
    }
};

// weight is the spec's kernel; taps counts the nonzero weights of every
// kernel the node rasterises for it (both bracketing entries when the radius
// is off the grid), i.e. the float32 terms it accumulates into a pixel.
struct SpecKernel
{
    OffsetTable weight;
    OffsetTable taps;
};

void addDisc(double radius, double share, int reach, std::vector<double>& weight,
             std::vector<double>& taps)
{
    const int w = 2 * reach + 1;
    const double inner = std::max(radius - 0.5 * kEdgeSoftnessPx, 0.0);
    const double outer = radius + 0.5 * kEdgeSoftnessPx;
    std::vector<double> disc(weight.size(), 0.0);
    double sum = 0.0;
    for (int y = -reach; y <= reach; ++y)
        for (int x = -reach; x <= reach; ++x) {
            const double d = std::sqrt(static_cast<double>(x * x + y * y));
            double v = 0.0;
            if (d <= inner)
                v = 1.0;
            else if (d < outer)
                v = (outer - d) / (outer - inner);
            disc[static_cast<size_t>(y + reach) * w + (x + reach)] = v;
            sum += v;
        }
    for (size_t i = 0; i < weight.size(); ++i) {
        weight[i] += share * disc[i] / sum;
        if (disc[i] > 0.0)
            taps[i] += 1.0;
    }
}

SpecKernel specKernel(double r)
{
    SpecKernel k;
    int reach = 0;
    std::vector<double> weight(1, 1.0);
    std::vector<double> taps(1, 1.0);
    if (r > kSharpRadiusPx) {
        double rA, rB, frac;
        gridBracket(r, rA, rB, frac);
        reach = static_cast<int>(std::ceil(rB + 0.5 * kEdgeSoftnessPx));
        weight.assign(static_cast<size_t>(2 * reach + 1) * (2 * reach + 1), 0.0);
        taps.assign(weight.size(), 0.0);
        addDisc(rA, 1.0 - frac, reach, weight, taps);
        if (frac > 0.0)
            addDisc(rB, frac, reach, weight, taps);
    }
    k.weight.build(reach, weight);
    k.taps.build(reach, taps);
    return k;
}

const double kGaussX[8] = { -0.9602898564975363, -0.7966664774136267, -0.5255324099163290,
                            -0.1834346424956498, 0.1834346424956498, 0.5255324099163290,
                            0.7966664774136267, 0.9602898564975363 };
const double kGaussW[8] = { 0.1012285362903763, 0.2223810344533745, 0.3137066458778873,
                            0.3626837833783620, 0.3626837833783620, 0.3137066458778873,
                            0.2223810344533745, 0.1012285362903763 };

// The overlap of the unit pixel [p + v, p + 1 + v) with [lo, hi) as a
// function of the shift v, a trapezoid, and its antiderivative in v.
struct Overlap
{
    double b[4];

    Overlap(double p, double lo, double hi) : b{ lo - p - 1.0, lo - p, hi - p - 1.0, hi - p } {}

    double value(double v) const
    {
        return std::max(0.0, std::min(v - b[0], std::min(1.0, b[3] - v)));
    }

    double integral(double v) const
    {
        if (v <= b[0])
            return 0.0;
        if (v <= b[1])
            return 0.5 * (v - b[0]) * (v - b[0]);
        if (v <= b[2])
            return 0.5 + (v - b[1]);
        if (v <= b[3])
            return 0.5 + (b[2] - b[1]) + (v - b[2]) - 0.5 * (v - b[2]) * (v - b[2]);
        return (b[2] - b[1]) + 1.0;
    }
};

// The share of the rays leaving pixel (px, py) through a lens disc of image
// radius r that land in the box: the disc-averaged area of the shifted pixel
// inside the box, (1 / pi r^2) * integral over the disc of ox(vx) oy(vy),
// with the vy integral closed-form and vx = r sin(theta) so the chord ends
// are smooth; theta is split wherever an overlap trapezoid kinks.
double exactCoverage(int px, int py, const double box[4], double r)
{
    const Overlap ox(px, box[0], box[2]);
    const Overlap oy(py, box[1], box[3]);
    if (!(r > 1e-12))
        return ox.value(0.0) * oy.value(0.0);
    if (px - r >= box[0] && px + 1 + r <= box[2] && py - r >= box[1] && py + 1 + r <= box[3])
        return 1.0;
    if (px + 1 + r <= box[0] || px - r >= box[2] || py + 1 + r <= box[1] || py - r >= box[3])
        return 0.0;
    std::vector<double> cuts = { -kPi / 2.0, kPi / 2.0 };
    for (double b : ox.b)
        if (std::fabs(b) < r)
            cuts.push_back(std::asin(b / r));
    for (double b : oy.b)
        if (std::fabs(b) < r) {
            const double t = std::acos(std::fabs(b) / r);
            cuts.push_back(t);
            cuts.push_back(-t);
        }
    std::sort(cuts.begin(), cuts.end());
    double sum = 0.0;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        const double half = 0.5 * (cuts[i + 1] - cuts[i]);
        if (!(half > 0.0))
            continue;
        const double mid = 0.5 * (cuts[i + 1] + cuts[i]);
        for (int g = 0; g < 8; ++g) {
            const double theta = mid + half * kGaussX[g];
            const double h = r * std::cos(theta);
            sum += half * kGaussW[g] * ox.value(r * std::sin(theta))
                 * (oy.integral(h) - oy.integral(-h)) * std::cos(theta);
        }
    }
    return sum / (kPi * r);
}

// A slice's coverage is the depth average of the thin-card coverage at each
// depth inside it, E[path length in the box] / thickness.
double sliceCoverage(int px, int py, const double box[4], const Lens& lens,
                     double z0, double z1)
{
    const double spread = std::fabs(std::fabs(lens.coc(z1)) - std::fabs(lens.coc(z0)));
    const int panels = std::max(2, static_cast<int>(std::ceil(spread / 0.1)));
    const double dz = (z1 - z0) / panels;
    double sum = 0.0;
    for (int p = 0; p < panels; ++p) {
        const double mid = z0 + (p + 0.5) * dz;
        for (int g = 0; g < 8; ++g)
            sum += 0.5 * kGaussW[g]
                 * exactCoverage(px, py, box, std::fabs(lens.coc(mid + 0.5 * dz * kGaussX[g])));
    }
    return sum / panels;
}

int kernelTerm(const Scene& s, const Lens& lens, double alpha, int wx0, int wy0, int wx1,
               int wy1, double step, int maxPieces, int argc, char** argv)
{
    const std::vector<Piece> pieces = cutPieces(lens, s.zF, s.zB, alpha, step, maxPieces);
    std::vector<SpecKernel> kernels;
    for (const Piece& p : pieces)
        kernels.push_back(specKernel(p.radius));
    const double cardRadius = std::fabs(lens.coc(s.bgZ));
    const SpecKernel cardKernel = specKernel(cardRadius);

    double minGap = INFINITY;
    for (size_t i = 0; i + 1 < pieces.size(); ++i)
        if ((pieces[i + 1].zFront < lens.focus) == (pieces[i].zBack <= lens.focus))
            minGap = std::min(minGap, std::fabs(pieces[i + 1].radius - pieces[i].radius));

    const int width = wx1 - wx0;
    const int height = wy1 - wy0;
    const int layers = static_cast<int>(pieces.size()) + (s.bgOn ? 1 : 0);
    std::vector<double> result(static_cast<size_t>(width) * height * 3, 0.0);

#pragma omp parallel for schedule(dynamic, 1)
    for (int py = wy0; py < wy1; ++py)
        for (int px = wx0; px < wx1; ++px) {
            double term = 0.0;
            double terms = 0.0;
            for (size_t i = 0; i < pieces.size(); ++i) {
                const double spec = kernels[i].weight.overBox(px, py, s.box);
                const double exact = sliceCoverage(px, py, s.box, lens, pieces[i].zFront,
                                                   pieces[i].zBack);
                term += pieces[i].alpha * std::fabs(spec - exact);
                terms += pieces[i].alpha * kernels[i].taps.overBox(px, py, s.box);
            }
            double card = 0.0;
            if (s.bgOn) {
                card = std::fabs(cardKernel.weight.overBox(px, py, s.bgBox)
                                 - exactCoverage(px, py, s.bgBox, cardRadius));
                terms += cardKernel.taps.overBox(px, py, s.bgBox);
            }
            double* o = &result[(static_cast<size_t>(py - wy0) * width + (px - wx0)) * 3];
            o[0] = term + card;
            o[1] = card;
            o[2] = kUnitRoundoff * (2.0 * terms + 2.0 * layers);
        }

    std::printf("#");
    for (int i = 1; i < argc; ++i)
        std::printf(" %s", argv[i]);
    std::printf("\n# step %.9g pieces %d minGap %.9g\n", step, static_cast<int>(pieces.size()),
                minGap);
    for (const Piece& p : pieces)
        std::printf("# piece z [%.9g, %.9g] alpha %.9g radius %.9g\n", p.zFront, p.zBack,
                    p.alpha, p.radius);
    for (int py = wy0; py < wy1; ++py)
        for (int px = wx0; px < wx1; ++px) {
            const double* o = &result[(static_cast<size_t>(py - wy0) * width + (px - wx0)) * 3];
            std::printf("%d %d %.9g %.9g %.9g\n", px, py, o[0], o[1], o[2]);
        }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const bool kernelMode = argc > 1 && std::strcmp(argv[1], "kernel") == 0;
    if (argc != (kernelMode ? 35 : 30)) {
        std::fprintf(stderr,
                     "usage: vref [kernel dMin dMax layers mergeTol] x0 y0 x1 y1 zF zB alpha "
                     "tintR tintG tintB size focus bgOn bx0 by0 bx1 by1 bz bgR bgG bgB "
                     "wx0 wy0 wx1 wy1 nSub nLens reps seed\n");
        return 2;
    }
    int k = kernelMode ? 2 : 1;
    auto num = [&]() { return std::atof(argv[k++]); };
    auto whole = [&]() { return std::atoi(argv[k++]); };
    double dMin = 0.0, dMax = 0.0, mergeTol = 0.0;
    int layers = 0;
    if (kernelMode) {
        dMin = num();
        dMax = num();
        layers = whole();
        mergeTol = num();
    }

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

    if (kernelMode) {
        if (!(dMax >= dMin && dMin > 0.0) || layers < 1) {
            std::fprintf(stderr, "vref kernel: need 0 < dMin <= dMax and layers >= 1\n");
            return 2;
        }
        return kernelTerm(s, lens, alpha, wx0, wy0, wx1, wy1,
                          pieceStepPx(lens, dMin, dMax, layers, mergeTol), layers + 1, argc, argv);
    }

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
