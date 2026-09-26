// Brute-force thin-lens reference for a dumped deep image.
//
//   thinlens_ref mc       dump size focus pixels nSub nLens reps seed [risers]
//   thinlens_ref analytic dump size focus pixels glPixel
//
// dump is a text file, one deep sample per line, "x y zFront zBack r g b a"
// (premultiplied colour, Nuke's bottom-up y); '#' lines are skipped.  size
// and focus are the node's manual-mode lens: signed CoC
// c(z) = size * (1 - focus / z), |c| the blur radius in pixels.  pixels is
// "x,y;x,y;...", "row:y:x0:x1" or "col:x:y0:y1" (inclusive), several joined
// with '+'.
//
// Lens model: a camera ray (o, u) has o = q + s, its position on the focal
// plane (q the output pixel's corner, s in [0,1)^2), and u uniform on the
// unit lens disc; at depth z it sits at image position o - c(z) u.  Scene
// model: a sample fills its source pixel's footprint [x,x+1) x [y,y+1) at
// depth (zFront + zBack) / 2.  A ray meets a sample when its position at the
// sample's depth lies in that footprint; the samples it meets are composited
// front to back with `over`, and the output pixel is their box-filtered mean.
//
// mc: nSub x nSub pixel strata times nLens x nLens lens strata (concentric
// map, so the strata stay equal-area), each jittered, repeated reps times
// with independent jitter; the pixel is the mean over the replicates and its
// standard error sd(replicate means) / sqrt(reps).  seed fixes the jitter,
// and each pixel draws from its own stream, so the output does not depend on
// the thread count.  risers (default 1) joins two edge-adjacent samples of
// one layer at different depths with the wall between their footprints, in
// the nearer sample's colour: a receding plane built from fronto-parallel
// footprints has gaps between its rows, and without the walls rays slip
// through them and an opaque plane reads alpha < 1.
//
// A layer is the set of samples sharing one RGBA.  mc output: '#' lines
// echoing the arguments and listing the layers, then per pixel
//   x y R G B A seR seG seB seA G/A seG/A R/A seR/A w0 sew0 w1 sew1 ...
// where wi is layer i's composited weight (its share of alpha).
//
// analytic cross-checks mc on scenes of cards: every constant-depth layer
// must be a filled axis-aligned rectangle, and at most one variable-depth
// layer is allowed, treated as a uniform background behind every card (a
// '# WARNING' line flags a pixel where it is not).  A card's lens set is the
// unit disc cut by an axis-aligned rectangle, so every exact hit set's area
// follows by inclusion-exclusion over rectangle intersections, integrated in
// double by Gauss-Legendre in theta with every kink a breakpoint; the pixel
// mean is glPixel x glPixel Gauss-Legendre over s.  It shares the loader and
// the lens model with mc, so it checks mc's integration, not the model.
//
// Build: any C++17 compiler; -fopenmp is optional.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Sample
{
    int x, y;
    double z;
    double c;
    std::array<double, 4> rgba;
    int layer;
};

struct Layer
{
    std::array<double, 4> rgba;
    double zMin = 1e300, zMax = -1e300;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    long count = 0;
};

struct Scene
{
    std::vector<Sample> samples;
    std::vector<Layer> layers;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    std::vector<std::vector<int>> grid;
    double size = 0, focus = 0;

    double coc(double z) const { return size * (1.0 - focus / z); }

    const std::vector<int>* at(int x, int y) const
    {
        if (x < bx0 || x >= bx1 || y < by0 || y >= by1)
            return nullptr;
        return &grid[static_cast<size_t>(y - by0) * static_cast<size_t>(bx1 - bx0)
                     + static_cast<size_t>(x - bx0)];
    }
};

Scene loadDump(const std::string& path, double size, double focus)
{
    Scene sc;
    sc.size = size;
    sc.focus = focus;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    std::string line;
    std::map<std::array<double, 4>, int> layerOf;
    int minX = 1 << 30, minY = 1 << 30, maxX = -(1 << 30), maxY = -(1 << 30);
    long volumetric = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ss(line);
        Sample s;
        double zf, zb;
        ss >> s.x >> s.y >> zf >> zb >> s.rgba[0] >> s.rgba[1] >> s.rgba[2] >> s.rgba[3];
        if (!ss)
            continue;
        if (zf != zb)
            ++volumetric;
        s.z = 0.5 * (zf + zb);
        s.c = sc.coc(s.z);
        auto it = layerOf.find(s.rgba);
        if (it == layerOf.end()) {
            const int id = static_cast<int>(sc.layers.size());
            layerOf[s.rgba] = id;
            Layer layer;
            layer.rgba = s.rgba;
            sc.layers.push_back(layer);
            it = layerOf.find(s.rgba);
        }
        s.layer = it->second;
        Layer& layer = sc.layers[static_cast<size_t>(s.layer)];
        layer.zMin = std::min(layer.zMin, s.z);
        layer.zMax = std::max(layer.zMax, s.z);
        layer.x0 = std::min(layer.x0, s.x);
        layer.y0 = std::min(layer.y0, s.y);
        layer.x1 = std::max(layer.x1, s.x + 1);
        layer.y1 = std::max(layer.y1, s.y + 1);
        ++layer.count;
        minX = std::min(minX, s.x);
        minY = std::min(minY, s.y);
        maxX = std::max(maxX, s.x);
        maxY = std::max(maxY, s.y);
        sc.samples.push_back(s);
    }
    if (sc.samples.empty()) {
        std::fprintf(stderr, "%s holds no samples\n", path.c_str());
        std::exit(2);
    }
    if (volumetric)
        std::fprintf(stderr, "warning: %ld volumetric samples treated at midpoint\n", volumetric);
    sc.bx0 = minX;
    sc.by0 = minY;
    sc.bx1 = maxX + 1;
    sc.by1 = maxY + 1;
    const size_t width = static_cast<size_t>(sc.bx1 - sc.bx0);
    sc.grid.assign(width * static_cast<size_t>(sc.by1 - sc.by0), {});
    for (size_t i = 0; i < sc.samples.size(); ++i) {
        const Sample& s = sc.samples[i];
        sc.grid[static_cast<size_t>(s.y - sc.by0) * width + static_cast<size_t>(s.x - sc.bx0)].push_back(
            static_cast<int>(i));
    }
    return sc;
}

std::vector<std::pair<int, int>> parsePixels(const std::string& spec)
{
    std::vector<std::pair<int, int>> out;
    std::stringstream parts(spec);
    std::string part;
    while (std::getline(parts, part, '+')) {
        if (part.rfind("row:", 0) == 0 || part.rfind("col:", 0) == 0) {
            int a, b0, b1;
            if (std::sscanf(part.c_str() + 4, "%d:%d:%d", &a, &b0, &b1) != 3)
                continue;
            for (int b = b0; b <= b1; ++b)
                out.push_back(part[0] == 'r' ? std::make_pair(b, a) : std::make_pair(a, b));
        } else {
            std::stringstream ps(part);
            std::string p;
            while (std::getline(ps, p, ';')) {
                int x, y;
                if (std::sscanf(p.c_str(), "%d,%d", &x, &y) == 2)
                    out.emplace_back(x, y);
            }
        }
    }
    return out;
}

struct Rng
{
    uint64_t state;

    explicit Rng(uint64_t seed) : state(seed) {}

    uint64_t next()
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
};

void concentric(double a, double b, double& ux, double& uy)
{
    const double sx = 2.0 * a - 1.0, sy = 2.0 * b - 1.0;
    if (sx == 0.0 && sy == 0.0) {
        ux = uy = 0.0;
        return;
    }
    double r, t;
    if (std::fabs(sx) > std::fabs(sy)) {
        r = sx;
        t = (kPi / 4.0) * (sy / sx);
    } else {
        r = sy;
        t = (kPi / 2.0) - (kPi / 4.0) * (sx / sy);
    }
    ux = r * std::cos(t);
    uy = r * std::sin(t);
}

struct Hit
{
    double z;
    int idx;
};

// Walks the image-space segment o - c u, c in [cLo, cHi], through the source
// grid (Amanatides-Woo) and keeps the samples the ray meets, sorted near to
// far.
void traceRay(const Scene& sc, double ox, double oy, double ux, double uy, double cLo, double cHi,
              bool risers, std::vector<Hit>& hits)
{
    hits.clear();
    const double x0 = ox - cLo * ux, y0 = oy - cLo * uy;
    const double x1 = ox - cHi * ux, y1 = oy - cHi * uy;
    int ix = static_cast<int>(std::floor(x0)), iy = static_cast<int>(std::floor(y0));
    const double dx = x1 - x0, dy = y1 - y0;
    const int stepX = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    const int stepY = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    double tMaxX = stepX ? ((stepX > 0 ? (ix + 1 - x0) : (x0 - ix)) / std::fabs(dx)) : 1e300;
    double tMaxY = stepY ? ((stepY > 0 ? (iy + 1 - y0) : (y0 - iy)) / std::fabs(dy)) : 1e300;
    const double tDX = stepX ? 1.0 / std::fabs(dx) : 1e300;
    const double tDY = stepY ? 1.0 / std::fabs(dy) : 1e300;
    int guard = 0;
    while (true) {
        const std::vector<int>* cell = sc.at(ix, iy);
        if (cell) {
            for (int i : *cell) {
                const Sample& s = sc.samples[static_cast<size_t>(i)];
                const double px = ox - s.c * ux, py = oy - s.c * uy;
                if (static_cast<int>(std::floor(px)) == ix && static_cast<int>(std::floor(py)) == iy)
                    hits.push_back({ s.z, i });
            }
        }
        const double t = std::min(tMaxX, tMaxY);
        if (t > 1.0 || ++guard > 100000)
            break;
        int nx = ix, ny = iy;
        if (tMaxX < tMaxY) {
            nx += stepX;
            tMaxX += tDX;
        } else {
            ny += stepY;
            tMaxY += tDY;
        }
        const std::vector<int>* next = sc.at(nx, ny);
        if (risers && cell && next) {
            const double cStar = cLo + t * (cHi - cLo);
            const double zStar = sc.focus / (1.0 - cStar / sc.size);
            for (int a : *cell)
                for (int b : *next) {
                    const Sample& sa = sc.samples[static_cast<size_t>(a)];
                    const Sample& sb = sc.samples[static_cast<size_t>(b)];
                    if (sa.layer != sb.layer || sa.z == sb.z)
                        continue;
                    const double zn = std::min(sa.z, sb.z), zf = std::max(sa.z, sb.z);
                    if (zStar > zn && zStar < zf)
                        hits.push_back({ zStar, sa.z < sb.z ? a : b });
                }
        }
        ix = nx;
        iy = ny;
    }
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.z < b.z || (a.z == b.z && a.idx < b.idx); });
}

void meanSe(const std::vector<double>& v, double& mean, double& se)
{
    const int reps = static_cast<int>(v.size());
    double s = 0, s2 = 0;
    for (double x : v) {
        s += x;
        s2 += x * x;
    }
    mean = s / reps;
    const double var = reps > 1 ? std::max(0.0, (s2 - reps * mean * mean) / (reps - 1)) : 0.0;
    se = std::sqrt(var / reps);
}

int runMc(int argc, char** argv)
{
    if (argc < 10)
        return 1;
    const Scene sc = loadDump(argv[2], std::atof(argv[3]), std::atof(argv[4]));
    const auto pixels = parsePixels(argv[5]);
    const int ns = std::atoi(argv[6]), nl = std::atoi(argv[7]), reps = std::atoi(argv[8]);
    const uint64_t seed = std::strtoull(argv[9], nullptr, 10);
    const bool risers = argc < 11 || std::atoi(argv[10]) != 0;
    const int nLayers = static_cast<int>(sc.layers.size());
    if (ns < 1 || nl < 1 || reps < 1)
        return 1;

    std::printf("# mc dump=%s size=%g focus=%g n_s=%d n_l=%d reps=%d seed=%llu risers=%d rays/pixel=%d\n",
                argv[2], sc.size, sc.focus, ns, nl, reps, static_cast<unsigned long long>(seed),
                static_cast<int>(risers), ns * ns * nl * nl * reps);
    for (int l = 0; l < nLayers; ++l) {
        const Layer& layer = sc.layers[static_cast<size_t>(l)];
        std::printf("# layer %d rgba %.9g %.9g %.9g %.9g z [%.6g, %.6g] bbox [%d,%d)-[%d,%d) count %ld\n", l,
                    layer.rgba[0], layer.rgba[1], layer.rgba[2], layer.rgba[3], layer.zMin, layer.zMax,
                    layer.x0, layer.y0, layer.x1, layer.y1, layer.count);
    }
    std::printf("# x y R G B A seR seG seB seA G/A seG/A R/A seR/A");
    for (int l = 0; l < nLayers; ++l)
        std::printf(" w%d sew%d", l, l);
    std::printf("\n");

    std::vector<std::string> lines(pixels.size());
#pragma omp parallel for schedule(dynamic, 1)
    for (long pi = 0; pi < static_cast<long>(pixels.size()); ++pi) {
        const int qx = pixels[static_cast<size_t>(pi)].first, qy = pixels[static_cast<size_t>(pi)].second;
        double cLo = 0.0, cHi = 0.0;
        for (const Sample& s : sc.samples) {
            const double r = std::fabs(s.c) + 2.0;
            if (std::fabs(s.x - qx) <= r && std::fabs(s.y - qy) <= r) {
                cLo = std::min(cLo, s.c);
                cHi = std::max(cHi, s.c);
            }
        }
        std::vector<Hit> hits;
        std::vector<std::array<double, 4>> repRgba(static_cast<size_t>(reps));
        std::vector<std::vector<double>> repW(static_cast<size_t>(reps),
                                              std::vector<double>(static_cast<size_t>(nLayers), 0.0));
        for (int r = 0; r < reps; ++r) {
            // splitmix64 streams whose raw seeds differ by its increment are
            // one stream shifted, so each replicate starts from a hashed
            // state or the replicates would be correlated.
            Rng mixer(seed ^ (static_cast<uint64_t>(qx) << 20) ^ (static_cast<uint64_t>(qy) << 40)
                      ^ (static_cast<uint64_t>(r) << 52));
            mixer.next();
            Rng rng(mixer.next()
                    ^ Rng(static_cast<uint64_t>(r) * 0xD1B54A32D192ED03ull + static_cast<uint64_t>(qx) * 31
                          + static_cast<uint64_t>(qy))
                          .next());
            std::array<double, 4> acc{ 0, 0, 0, 0 };
            std::vector<double>& w = repW[static_cast<size_t>(r)];
            long n = 0;
            for (int si = 0; si < ns; ++si)
                for (int sj = 0; sj < ns; ++sj)
                    for (int li = 0; li < nl; ++li)
                        for (int lj = 0; lj < nl; ++lj) {
                            const double sx = (si + rng.uniform()) / ns, sy = (sj + rng.uniform()) / ns;
                            double ux, uy;
                            concentric((li + rng.uniform()) / nl, (lj + rng.uniform()) / nl, ux, uy);
                            traceRay(sc, qx + sx, qy + sy, ux, uy, cLo, cHi, risers, hits);
                            double transmit = 1.0;
                            for (const Hit& h : hits) {
                                const Sample& s = sc.samples[static_cast<size_t>(h.idx)];
                                for (int c = 0; c < 4; ++c)
                                    acc[static_cast<size_t>(c)] += transmit * s.rgba[static_cast<size_t>(c)];
                                w[static_cast<size_t>(s.layer)] += transmit * s.rgba[3];
                                transmit *= (1.0 - s.rgba[3]);
                                if (transmit <= 0.0)
                                    break;
                            }
                            ++n;
                        }
            for (int c = 0; c < 4; ++c)
                repRgba[static_cast<size_t>(r)][static_cast<size_t>(c)] = acc[static_cast<size_t>(c)] / double(n);
            for (int l = 0; l < nLayers; ++l)
                w[static_cast<size_t>(l)] /= double(n);
        }
        std::vector<double> v(static_cast<size_t>(reps));
        const auto summarise = [&](auto get, double& mean, double& se) {
            for (int r = 0; r < reps; ++r)
                v[static_cast<size_t>(r)] = get(repRgba[static_cast<size_t>(r)], r);
            meanSe(v, mean, se);
        };
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(7);
        os << qx << ' ' << qy;
        double m[4], e[4];
        for (int c = 0; c < 4; ++c)
            summarise([c](const std::array<double, 4>& p, int) { return p[static_cast<size_t>(c)]; }, m[c], e[c]);
        for (int c = 0; c < 4; ++c)
            os << ' ' << m[c];
        for (int c = 0; c < 4; ++c)
            os << ' ' << e[c];
        double ga, gaSe, ra, raSe;
        summarise([](const std::array<double, 4>& p, int) { return p[3] > 0 ? p[1] / p[3] : 0.0; }, ga, gaSe);
        summarise([](const std::array<double, 4>& p, int) { return p[3] > 0 ? p[0] / p[3] : 0.0; }, ra, raSe);
        os << ' ' << ga << ' ' << gaSe << ' ' << ra << ' ' << raSe;
        for (int l = 0; l < nLayers; ++l) {
            double wm, we;
            summarise([&](const std::array<double, 4>&, int r) {
                return repW[static_cast<size_t>(r)][static_cast<size_t>(l)];
            }, wm, we);
            os << ' ' << wm << ' ' << we;
        }
        lines[static_cast<size_t>(pi)] = os.str();
    }
    for (const std::string& l : lines)
        std::printf("%s\n", l.c_str());
    return 0;
}

struct GaussLegendre
{
    std::vector<double> x, w;

    explicit GaussLegendre(int n) : x(static_cast<size_t>(n)), w(static_cast<size_t>(n))
    {
        for (int i = 0; i < n; ++i) {
            double z = std::cos(kPi * (i + 0.75) / (n + 0.5)), pp = 0;
            for (int it = 0; it < 100; ++it) {
                double p1 = 1.0, p2 = 0.0;
                for (int j = 1; j <= n; ++j) {
                    const double p3 = p2;
                    p2 = p1;
                    p1 = ((2.0 * j - 1.0) * z * p2 - (j - 1.0) * p3) / j;
                }
                pp = n * (z * p1 - p2) / (z * z - 1.0);
                const double z1 = z;
                z = z1 - p1 / pp;
                if (std::fabs(z - z1) < 1e-15)
                    break;
            }
            x[static_cast<size_t>(i)] = z;
            w[static_cast<size_t>(i)] = 2.0 / ((1.0 - z * z) * pp * pp);
        }
    }
};

// Area of the unit disc cut by [a0,a1] x [b0,b1].
double discRectArea(double a0, double a1, double b0, double b1)
{
    static const GaussLegendre gl(40);
    if (a1 <= a0 || b1 <= b0)
        return 0.0;
    const double lo = std::max(a0, -1.0), hi = std::min(a1, 1.0);
    if (hi <= lo)
        return 0.0;
    const double tLo = std::asin(lo), tHi = std::asin(hi);
    std::vector<double> cuts{ tLo, tHi };
    for (double b : { b0, b1 }) {
        const double ab = std::fabs(b);
        if (ab < 1.0) {
            const double t = std::acos(ab);
            for (double tt : { t, -t })
                if (tt > tLo && tt < tHi)
                    cuts.push_back(tt);
        }
    }
    std::sort(cuts.begin(), cuts.end());
    double area = 0.0;
    for (size_t k = 0; k + 1 < cuts.size(); ++k) {
        const double t0 = cuts[k], t1 = cuts[k + 1];
        if (t1 <= t0)
            continue;
        const double mid = 0.5 * (t0 + t1), half = 0.5 * (t1 - t0);
        for (size_t i = 0; i < gl.x.size(); ++i) {
            const double t = mid + half * gl.x[i];
            const double h = std::cos(t);
            const double len = std::min(b1, h) - std::max(b0, -h);
            if (len > 0)
                area += gl.w[i] * half * h * len;
        }
    }
    return area;
}

struct Rect
{
    double a0, a1, b0, b1;
};

// The lens-space set of a card [x0,x1) x [y0,y1) at signed CoC c, seen from
// ray origin o.
Rect lensRect(double x0, double x1, double y0, double y1, double c, double ox, double oy)
{
    Rect r;
    if (c < 0) {
        const double m = -c;
        r.a0 = (x0 - ox) / m;
        r.a1 = (x1 - ox) / m;
        r.b0 = (y0 - oy) / m;
        r.b1 = (y1 - oy) / m;
    } else {
        r.a0 = (ox - x1) / c;
        r.a1 = (ox - x0) / c;
        r.b0 = (oy - y1) / c;
        r.b1 = (oy - y0) / c;
    }
    return r;
}

int popcount(unsigned v)
{
    int n = 0;
    for (; v; v &= v - 1)
        ++n;
    return n;
}

int runAnalytic(int argc, char** argv)
{
    if (argc < 7)
        return 1;
    const Scene sc = loadDump(argv[2], std::atof(argv[3]), std::atof(argv[4]));
    const auto pixels = parsePixels(argv[5]);
    const int nq = std::atoi(argv[6]);
    if (nq < 1)
        return 1;
    const GaussLegendre glp(nq);

    std::vector<int> cards;
    int background = -1;
    for (int l = 0; l < static_cast<int>(sc.layers.size()); ++l) {
        const Layer& layer = sc.layers[static_cast<size_t>(l)];
        const long area = static_cast<long>(layer.x1 - layer.x0) * static_cast<long>(layer.y1 - layer.y0);
        if (layer.zMin == layer.zMax) {
            if (area != layer.count) {
                std::fprintf(stderr, "layer %d is constant-depth but not a filled rectangle (%ld of %ld)\n", l,
                             layer.count, area);
                return 3;
            }
            cards.push_back(l);
        } else {
            if (background >= 0) {
                std::fprintf(stderr, "more than one variable-depth layer\n");
                return 3;
            }
            background = l;
        }
    }
    std::sort(cards.begin(), cards.end(), [&](int a, int b) {
        return sc.layers[static_cast<size_t>(a)].zMin < sc.layers[static_cast<size_t>(b)].zMin;
    });
    const int nc = static_cast<int>(cards.size());
    std::printf("# analytic dump=%s size=%g focus=%g gl_pixel=%d cards=%d background=%d\n", argv[2], sc.size,
                sc.focus, nq, nc, background);
    for (int i = 0; i < nc; ++i) {
        const Layer& layer = sc.layers[static_cast<size_t>(cards[static_cast<size_t>(i)])];
        std::printf("# card %d (layer %d) z %.9g c %.6f rect [%d,%d)x[%d,%d) rgba %.9g %.9g %.9g %.9g\n", i,
                    cards[static_cast<size_t>(i)], layer.zMin, sc.coc(layer.zMin), layer.x0, layer.x1,
                    layer.y0, layer.y1, layer.rgba[0], layer.rgba[1], layer.rgba[2], layer.rgba[3]);
    }
    std::array<double, 4> bg{ 0, 0, 0, 0 };
    if (background >= 0)
        bg = sc.layers[static_cast<size_t>(background)].rgba;

    const int nSets = 1 << nc;
    std::vector<std::array<double, 4>> setRgba(static_cast<size_t>(nSets));
    std::vector<std::vector<double>> setW(static_cast<size_t>(nSets),
                                          std::vector<double>(static_cast<size_t>(nc + 1), 0.0));
    for (int set = 0; set < nSets; ++set) {
        std::array<double, 4> acc{ 0, 0, 0, 0 };
        double transmit = 1.0;
        for (int i = 0; i < nc; ++i)
            if (set & (1 << i)) {
                const auto& c = sc.layers[static_cast<size_t>(cards[static_cast<size_t>(i)])].rgba;
                for (int k = 0; k < 4; ++k)
                    acc[static_cast<size_t>(k)] += transmit * c[static_cast<size_t>(k)];
                setW[static_cast<size_t>(set)][static_cast<size_t>(i)] = transmit * c[3];
                transmit *= 1.0 - c[3];
            }
        for (int k = 0; k < 4; ++k)
            acc[static_cast<size_t>(k)] += transmit * bg[static_cast<size_t>(k)];
        setW[static_cast<size_t>(set)][static_cast<size_t>(nc)] = transmit * bg[3];
        setRgba[static_cast<size_t>(set)] = acc;
    }

    std::printf("# x y | centre: R G B A G/A R/A | pixel mean: R G B A G/A R/A | pixel-mean weights card0..card%d bg"
                " | centre nesting i/j: area(lens_i & lens_j)/area(lens_i)\n",
                nc - 1);
    for (const auto& [qx, qy] : pixels) {
        if (background >= 0) {
            double maxCardZ = 0;
            for (int i : cards)
                maxCardZ = std::max(maxCardZ, sc.layers[static_cast<size_t>(i)].zMax);
            double minBgZ = 1e300;
            for (const Sample& s : sc.samples)
                if (s.layer == background) {
                    const double r = std::fabs(s.c) + 2.0;
                    if (std::fabs(s.x - qx) <= r && std::fabs(s.y - qy) <= r)
                        minBgZ = std::min(minBgZ, s.z);
                }
            if (!(minBgZ > maxCardZ))
                std::printf("# WARNING pixel %d,%d: background reaches z=%.4f in front of a card (z<=%.4f)\n", qx,
                            qy, minBgZ, maxCardZ);
        }
        const auto evalAt = [&](double ox, double oy, std::array<double, 4>& rgba, std::vector<double>& w) {
            std::vector<Rect> rects(static_cast<size_t>(nc));
            for (int i = 0; i < nc; ++i) {
                const Layer& layer = sc.layers[static_cast<size_t>(cards[static_cast<size_t>(i)])];
                rects[static_cast<size_t>(i)] =
                    lensRect(layer.x0, layer.x1, layer.y0, layer.y1, sc.coc(layer.zMin), ox, oy);
            }
            std::vector<double> inter(static_cast<size_t>(nSets));
            for (int set = 0; set < nSets; ++set) {
                Rect r{ -1, 1, -1, 1 };
                for (int i = 0; i < nc; ++i)
                    if (set & (1 << i)) {
                        const Rect& ri = rects[static_cast<size_t>(i)];
                        r.a0 = std::max(r.a0, ri.a0);
                        r.a1 = std::min(r.a1, ri.a1);
                        r.b0 = std::max(r.b0, ri.b0);
                        r.b1 = std::min(r.b1, ri.b1);
                    }
                inter[static_cast<size_t>(set)] = discRectArea(r.a0, r.a1, r.b0, r.b1) / kPi;
            }
            rgba = { 0, 0, 0, 0 };
            w.assign(static_cast<size_t>(nc + 1), 0.0);
            for (int set = 0; set < nSets; ++set) {
                double a = 0;
                for (int super = set; super < nSets; super = (super + 1) | set) {
                    const int extra = popcount(static_cast<unsigned>(super & ~set));
                    a += (extra & 1 ? -1.0 : 1.0) * inter[static_cast<size_t>(super)];
                }
                for (int k = 0; k < 4; ++k)
                    rgba[static_cast<size_t>(k)] += a * setRgba[static_cast<size_t>(set)][static_cast<size_t>(k)];
                for (int i = 0; i <= nc; ++i)
                    w[static_cast<size_t>(i)] += a * setW[static_cast<size_t>(set)][static_cast<size_t>(i)];
            }
        };
        std::array<double, 4> cRgba;
        std::vector<double> cW;
        evalAt(qx + 0.5, qy + 0.5, cRgba, cW);
        std::array<double, 4> mRgba{ 0, 0, 0, 0 };
        std::vector<double> mW(static_cast<size_t>(nc + 1), 0.0);
        for (size_t i = 0; i < glp.x.size(); ++i)
            for (size_t j = 0; j < glp.x.size(); ++j) {
                std::array<double, 4> r;
                std::vector<double> w;
                evalAt(qx + 0.5 + 0.5 * glp.x[i], qy + 0.5 + 0.5 * glp.x[j], r, w);
                const double ww = 0.25 * glp.w[i] * glp.w[j];
                for (int k = 0; k < 4; ++k)
                    mRgba[static_cast<size_t>(k)] += ww * r[static_cast<size_t>(k)];
                for (int k = 0; k <= nc; ++k)
                    mW[static_cast<size_t>(k)] += ww * w[static_cast<size_t>(k)];
            }
        std::printf("%d %d |", qx, qy);
        for (int k = 0; k < 4; ++k)
            std::printf(" %.8f", cRgba[static_cast<size_t>(k)]);
        std::printf(" %.8f %.8f |", cRgba[3] > 0 ? cRgba[1] / cRgba[3] : 0.0, cRgba[3] > 0 ? cRgba[0] / cRgba[3] : 0.0);
        for (int k = 0; k < 4; ++k)
            std::printf(" %.8f", mRgba[static_cast<size_t>(k)]);
        std::printf(" %.8f %.8f |", mRgba[3] > 0 ? mRgba[1] / mRgba[3] : 0.0, mRgba[3] > 0 ? mRgba[0] / mRgba[3] : 0.0);
        for (int k = 0; k <= nc; ++k)
            std::printf(" %.8f", mW[static_cast<size_t>(k)]);
        std::printf(" |");
        for (int i = 0; i < nc; ++i)
            for (int j = i + 1; j < nc; ++j) {
                const Layer& li = sc.layers[static_cast<size_t>(cards[static_cast<size_t>(i)])];
                const Layer& lj = sc.layers[static_cast<size_t>(cards[static_cast<size_t>(j)])];
                const Rect ri = lensRect(li.x0, li.x1, li.y0, li.y1, sc.coc(li.zMin), qx + 0.5, qy + 0.5);
                const Rect rj = lensRect(lj.x0, lj.x1, lj.y0, lj.y1, sc.coc(lj.zMin), qx + 0.5, qy + 0.5);
                const double ai = discRectArea(ri.a0, ri.a1, ri.b0, ri.b1) / kPi;
                const double aij = discRectArea(std::max(ri.a0, rj.a0), std::min(ri.a1, rj.a1),
                                                std::max(ri.b0, rj.b0), std::min(ri.b1, rj.b1))
                                 / kPi;
                std::printf(" %d/%d:%.6f/%.6f", i, j, aij, ai);
            }
        std::printf("\n");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string(argv[1]) == "mc")
        return runMc(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "analytic")
        return runAnalytic(argc, argv);
    std::fprintf(stderr,
                 "usage:\n  %s mc <dump> <size> <focus> <pixels> <n_s> <n_l> <reps> <seed> [risers=1]\n"
                 "  %s analytic <dump> <size> <focus> <pixels> <gl_pixel>\n",
                 argv[0], argv[0]);
    return 1;
}
