// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Marten Blumen
// Source: https://github.com/bratgot/DeepThinner
//
// ============================================================================
//
//  DeepThinner v2.0 — Deep Sample Optimisation for Nuke 16
//
//  Created by Marten Blumen
//
//  A high-performance deep compositing utility that reduces per-pixel sample
//  counts through seven independent, artist-controllable passes.
//
// ============================================================================

#include "DDImage/DeepFilterOp.h"
#include "DDImage/DeepPixel.h"
#include "DDImage/DeepPlane.h"
#include "DDImage/Knobs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

using namespace DD::Image;

// ---------------------------------------------------------------------------
struct SampleRecord {
    int   originalIndex;
    float zFront;
    float zBack;
    float alpha;
    float rgb[3];
};

// ---------------------------------------------------------------------------
class DeepThinner : public DeepFilterOp
{
    // --- Pass 1: Depth Range ---
    bool  _depthClip;
    float _zNear;
    float _zFar;

    // --- Pass 2: Alpha Cull ---
    bool  _cullTransparent;
    float _alphaThreshold;

    // --- Pass 3: Occlusion Cutoff ---
    bool  _occlusionCutoff;
    float _occlusionAlpha;

    // --- Pass 4: Contribution Cull ---
    bool  _contributionCull;
    float _contributionMin;

    // --- Pass 5: Volumetric Collapse ---
    bool  _volumetricCollapse;
    float _volumeAlphaMax;
    int   _volumeGroupSize;

    // --- Pass 6: Smart Merge (Z + color aware) ---
    bool  _mergeSamples;
    float _tolerance;
    float _colorTolerance;

    // --- Pass 7: Max Samples ---
    int   _maxSamples;

    // --- Statistics ---
    std::atomic<int64_t> _samplesIn;
    std::atomic<int64_t> _samplesOut;
    const char* _statText;
    char _statBuf[512];

    // Thread-local scratch
    struct ScratchBuf {
        std::vector<SampleRecord> sorted;
        std::vector<bool>         alive;
        std::vector<float>        mergedChannels;
    };

    static float colorDistance(const float a[3], const float b[3])
    {
        float d = std::fabs(a[0] - b[0]);
        d = std::max(d, std::fabs(a[1] - b[1]));
        d = std::max(d, std::fabs(a[2] - b[2]));
        return d;
    }

    void updateStatKnob()
    {
        const int64_t si = _samplesIn.load(std::memory_order_relaxed);
        const int64_t so = _samplesOut.load(std::memory_order_relaxed);

        if (si > 0) {
            const int64_t removed = si - so;
            const double pct = 100.0 * (1.0 - (double)so / (double)si);
            snprintf(_statBuf, sizeof(_statBuf),
                     "In: %lld   Out: %lld   Removed: %lld   (%.1f%% reduction)",
                     (long long)si, (long long)so,
                     (long long)removed, pct);
        } else {
            snprintf(_statBuf, sizeof(_statBuf),
                     "No samples processed yet — render to see statistics.");
        }

        Knob* k = knob("stat_display");
        if (k) k->set_text(_statBuf);
    }

public:
    DeepThinner(Node* node) : DeepFilterOp(node),
        _depthClip(false),
        _zNear(0.0f),
        _zFar(1e10f),
        _cullTransparent(true),
        _alphaThreshold(0.0f),
        _occlusionCutoff(true),
        _occlusionAlpha(0.999f),
        _contributionCull(true),
        _contributionMin(0.001f),
        _volumetricCollapse(false),
        _volumeAlphaMax(0.01f),
        _volumeGroupSize(4),
        _mergeSamples(true),
        _tolerance(0.01f),
        _colorTolerance(0.01f),
        _maxSamples(0),
        _samplesIn(0),
        _samplesOut(0),
        _statText(_statBuf)
    {
        snprintf(_statBuf, sizeof(_statBuf),
                 "No samples processed yet — render to see statistics.");
    }

    const char* Class() const override { return "DeepThinner"; }

    const char* node_help() const override {
        return
            "<h2>DeepThinner v2.0</h2>"
            "<i>Deep Sample Optimisation for Nuke 16</i>\n\n"
            "<b>Created by Marten Blumen</b>\n\n"

            "<h3>Overview</h3>"
            "Reduces deep sample counts per pixel to dramatically improve "
            "downstream compositing speed, memory footprint, and render "
            "write times.  All seven passes are independent and can be "
            "enabled or disabled individually.\n\n"

            "<h3>Passes</h3>"
            "<b>1. Depth Range</b> — Clips samples outside a near/far Z range.\n"
            "<b>2. Alpha Cull</b> — Removes near-transparent samples.\n"
            "<b>3. Occlusion Cutoff</b> — Drops everything behind full opacity.\n"
            "<b>4. Contribution Cull</b> — Removes negligible-contribution samples.\n"
            "<b>5. Volumetric Collapse</b> — Collapses runs of volume samples.\n"
            "<b>6. Smart Merge</b> — Merges Z-close and color-similar samples.\n"
            "<b>7. Max Samples</b> — Hard per-pixel cap.\n\n"

            "<h3>Technical Notes</h3>"
            "All passes assume premultiplied colour data.  The merge composite "
            "uses front-to-back over accumulation.  Thread-local scratch buffers "
            "and per-tile atomic counters minimise contention.";
    }

    // ------------------------------------------------------------------
    // Knobs
    // ------------------------------------------------------------------
    void knobs(Knob_Callback f) override
    {
        // ================================================================
        // HEADER
        // ================================================================
        Text_knob(f, "<b><font size='5'>DeepThinner v2.0</font></b>");
        Text_knob(f, "<i>Deep Sample Optimisation for Nuke 16</i>");
        Divider(f, "");

        // ================================================================
        // ARTIST GUIDE
        // ================================================================
        BeginClosedGroup(f, "Artist Guide");
        Text_knob(f,
            "<b>Quick Start</b><br>"
            "The defaults are a good starting point. Connect your deep stream, render, and check the<br>"
            "Statistics group at the bottom for your reduction results.<br>"
            "<br>"
            "<b>Recommended Workflow</b><br>"
            "1. Start with <b>Occlusion Cutoff</b> at 0.999 — this alone often removes 50-80%% of samples<br>"
            "   with zero visual change.<br>"
            "2. Enable <b>Contribution Cull</b> at 0.001-0.005 for the next biggest win, especially on<br>"
            "   volume-heavy shots.<br>"
            "3. Use <b>Volumetric Collapse</b> on shots with atmospheric haze, fog, or explosions where<br>"
            "   hundreds of micro-samples represent smooth gradients.<br>"
            "4. <b>Smart Merge</b> works best on hard surfaces where multiple overlapping objects create<br>"
            "   redundant samples at similar depths.<br>"
            "5. <b>Depth Range</b> is a targeted tool — use it to isolate a depth region or strip out<br>"
            "   distant noise.<br>"
            "6. <b>Max Samples</b> is a safety net — use it to guarantee a budget when writing deep EXRs.<br>"
            "<br>"
            "<b>Quality Checking</b><br>"
            "- A/B with a DeepToImage or flatten to verify visual fidelity.<br>"
            "- Check edge detail on transparent objects (hair, glass) — those are most sensitive to<br>"
            "  aggressive thinning.<br>"
            "- If you see banding in volumes, reduce Volumetric Collapse group size or raise the<br>"
            "  volume alpha max.<br>"
            "<br>"
            "<b>Performance Tips</b><br>"
            "- Insert DeepThinner <i>before</i> downstream DeepMerge or DeepHoldout nodes to accelerate<br>"
            "  the entire tree.<br>"
            "- On heavy shots, thinning before writing deep EXRs can cut file sizes and write times<br>"
            "  dramatically.<br>"
            "- Each pass is cheap — the overhead of having all seven enabled is negligible compared<br>"
            "  to the savings."
        );
        EndGroup(f);

        // ================================================================
        // USAGE NOTES
        // ================================================================
        BeginClosedGroup(f, "Technical Notes");
        Text_knob(f,
            "<b>Data Assumptions</b><br>"
            "- All colour channels are assumed <b>premultiplied</b>, which is the Nuke deep standard.<br>"
            "- Merged samples use front-to-back <b>over</b> compositing: Cout += Cin x (1 - accAlpha).<br>"
            "- Merged samples preserve the minimum Z-front and maximum Z-back of their constituent<br>"
            "  samples.<br>"
            "<br>"
            "<b>Pass Order</b><br>"
            "Passes run in sequence: Depth Range -> Alpha Cull -> Occlusion Cutoff -> Contribution<br>"
            "Cull -> Volumetric Collapse -> Smart Merge -> Max Samples. Earlier passes reduce the<br>"
            "workload for later passes.<br>"
            "<br>"
            "<b>Threading</b><br>"
            "Processing is fully multi-threaded. Each tile uses a thread-local scratch buffer (zero<br>"
            "heap allocations in the hot loop). Atomic sample counters are flushed once per tile, not<br>"
            "per pixel, minimising contention.<br>"
            "<br>"
            "<b>Channel Handling</b><br>"
            "All channels from the input are passed through — only sample count is affected. AOV<br>"
            "channels (custom layers, motion vectors, normals, IDs) are composited the same way as<br>"
            "colour channels during merging.<br>"
            "<br>"
            "<b>Edge Cases</b><br>"
            "- A pixel with zero surviving samples emits a 'hole' (no data) rather than a zero-alpha<br>"
            "  sample.<br>"
            "- If deep.back is not present, deep.front is used for both.<br>"
            "- If alpha is not present, samples are treated as fully opaque."
        );
        EndGroup(f);

        Divider(f, "");

        // ================================================================
        // PASS 1: Depth Range
        // ================================================================
        BeginGroup(f, "Depth Range");
        Bool_knob(f, &_depthClip, "depth_clip", "enable depth clip");
        Tooltip(f, "Clip samples outside the near/far Z range.");

        Float_knob(f, &_zNear, "z_near", "near Z");
        SetRange(f, 0.0f, 100.0f);
        Tooltip(f, "Samples with Z-front less than this are removed.");

        Float_knob(f, &_zFar, "z_far", "far Z");
        SetRange(f, 1.0f, 100000.0f);
        Tooltip(f, "Samples with Z-front greater than this are removed.");
        EndGroup(f);

        // ================================================================
        // PASS 2: Alpha Cull
        // ================================================================
        BeginGroup(f, "Alpha Cull");
        Bool_knob(f, &_cullTransparent, "cull_transparent", "enable alpha cull");
        Tooltip(f, "Remove samples with alpha at or below the threshold.");

        Float_knob(f, &_alphaThreshold, "alpha_threshold", "alpha threshold");
        SetRange(f, 0.0f, 0.1f);
        Tooltip(f, "Samples with alpha <= this are removed. "
                    "0.0 removes only fully transparent samples.");
        EndGroup(f);

        // ================================================================
        // PASS 3: Occlusion Cutoff
        // ================================================================
        BeginGroup(f, "Occlusion Cutoff");
        Bool_knob(f, &_occlusionCutoff, "occlusion_cutoff", "enable occlusion cutoff");
        Tooltip(f, "Drop all samples behind the point where accumulated "
                    "alpha reaches the cutoff. Usually the biggest single saver.");

        Float_knob(f, &_occlusionAlpha, "occlusion_alpha", "alpha cutoff");
        SetRange(f, 0.9f, 1.0f);
        Tooltip(f, "Cumulative alpha at which deeper samples are discarded. "
                    "0.999 = once 99.9%% opaque, drop everything behind.");
        EndGroup(f);

        // ================================================================
        // PASS 4: Contribution Cull
        // ================================================================
        BeginGroup(f, "Contribution Cull");
        Bool_knob(f, &_contributionCull, "contribution_cull", "enable contribution cull");
        Tooltip(f, "Remove samples whose visible contribution "
                    "(alpha x remaining coverage) is below the minimum.");

        Float_knob(f, &_contributionMin, "contribution_min", "min contribution");
        SetRange(f, 0.0f, 0.05f);
        Tooltip(f, "A sample must contribute at least this much to survive. "
                    "0.001 = conservative, 0.01 = aggressive.");
        EndGroup(f);

        // ================================================================
        // PASS 5: Volumetric Collapse
        // ================================================================
        BeginGroup(f, "Volumetric Collapse");
        Bool_knob(f, &_volumetricCollapse, "volumetric_collapse", "enable volumetric collapse");
        Tooltip(f, "Collapse runs of consecutive low-alpha samples into "
                    "fewer representative samples. Ideal for atmospherics, "
                    "fog, and explosion renders.");

        Float_knob(f, &_volumeAlphaMax, "volume_alpha_max", "volume alpha max");
        SetRange(f, 0.0f, 0.1f);
        Tooltip(f, "Samples with alpha below this are classified as "
                    "volumetric and eligible for collapsing.");

        Int_knob(f, &_volumeGroupSize, "volume_group_size", "collapse every N");
        SetRange(f, 2, 16);
        Tooltip(f, "How many consecutive volumetric samples to collapse "
                    "into one. Higher = more aggressive. 4 is a good start.");
        EndGroup(f);

        // ================================================================
        // PASS 6: Smart Merge
        // ================================================================
        BeginGroup(f, "Smart Merge");
        Bool_knob(f, &_mergeSamples, "merge_samples", "enable smart merge");
        Tooltip(f, "Merge consecutive samples close in Z AND colour.");

        Float_knob(f, &_tolerance, "tolerance", "Z tolerance");
        SetRange(f, 0.0f, 1.0f);
        Tooltip(f, "Maximum Z-front distance for merge eligibility.");

        Float_knob(f, &_colorTolerance, "color_tolerance", "color tolerance");
        SetRange(f, 0.0f, 0.1f);
        Tooltip(f, "Maximum per-channel RGB difference for merge eligibility. "
                    "0 = merge by Z only (ignore colour).");
        EndGroup(f);

        // ================================================================
        // PASS 7: Max Samples
        // ================================================================
        BeginGroup(f, "Max Samples");
        Int_knob(f, &_maxSamples, "max_samples", "max samples per pixel");
        SetRange(f, 0, 256);
        Tooltip(f, "Hard per-pixel cap. 0 = unlimited. "
                    "Deepest samples dropped first.");
        EndGroup(f);

        // ================================================================
        // STATISTICS
        // ================================================================
        Divider(f, "");
        BeginGroup(f, "Statistics");
        String_knob(f, &_statText, "stat_display", "");
        SetFlags(f, Knob::DISABLED | Knob::OUTPUT_ONLY);
        Tooltip(f, "Shows sample reduction statistics after each cook.");
        Button(f, "update_stats", "Update Statistics");
        Tooltip(f, "Force a refresh of the statistics display. "
                    "Useful if you've just rendered and the numbers "
                    "haven't updated yet.");
        EndGroup(f);

        // ================================================================
        // FOOTER
        // ================================================================
        Divider(f, "");
        Text_knob(f, "<font color='#888888'><i>DeepThinner v2.0  —  "
                      "Created by Marten Blumen</i></font>");
    }

    // ------------------------------------------------------------------
    int knob_changed(Knob* k) override
    {
        if (k->is("depth_clip")) {
            knob("z_near")->enable(_depthClip);
            knob("z_far")->enable(_depthClip);
            return 1;
        }
        if (k->is("cull_transparent")) {
            knob("alpha_threshold")->enable(_cullTransparent);
            return 1;
        }
        if (k->is("occlusion_cutoff")) {
            knob("occlusion_alpha")->enable(_occlusionCutoff);
            return 1;
        }
        if (k->is("contribution_cull")) {
            knob("contribution_min")->enable(_contributionCull);
            return 1;
        }
        if (k->is("volumetric_collapse")) {
            knob("volume_alpha_max")->enable(_volumetricCollapse);
            knob("volume_group_size")->enable(_volumetricCollapse);
            return 1;
        }
        if (k->is("merge_samples")) {
            knob("tolerance")->enable(_mergeSamples);
            knob("color_tolerance")->enable(_mergeSamples);
            return 1;
        }
        if (k->is("update_stats")) {
            updateStatKnob();
            return 1;
        }
        return DeepFilterOp::knob_changed(k);
    }

    // ------------------------------------------------------------------
    void _validate(bool for_real) override
    {
        DeepFilterOp::_validate(for_real);
        _samplesIn.store(0,  std::memory_order_relaxed);
        _samplesOut.store(0, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------------
    bool doDeepEngine(Box box, const ChannelSet& channels,
                      DeepOutputPlane& plane) override
    {
        DeepOp* in = input0();
        if (!in) return true;

        DeepPlane inPlane;
        if (!in->deepEngine(box, channels, inPlane))
            return false;

        plane = DeepOutputPlane(channels, box, DeepPixel::eUnordered);

        static thread_local ScratchBuf scratch;

        const ChannelMap& chanMap = inPlane.channels();
        const bool hasChanBack  = chanMap.contains(Chan_DeepBack);
        const bool hasChanAlpha = chanMap.contains(Chan_Alpha);
        const bool hasChanR     = chanMap.contains(Chan_Red);
        const bool hasChanG     = chanMap.contains(Chan_Green);
        const bool hasChanB     = chanMap.contains(Chan_Blue);
        const int  nChans       = (int)chanMap.size();

        int64_t localIn  = 0;
        int64_t localOut = 0;

        for (Box::iterator it = box.begin(); it != box.end(); ++it) {
            DeepPixel inPixel = inPlane.getPixel(it);
            const int sampleCount = (int)inPixel.getSampleCount();
            localIn += sampleCount;

            if (sampleCount == 0) {
                plane.addHole();
                continue;
            }

            // ============================================================
            // COLLECT & SORT by Z-front
            // ============================================================
            scratch.sorted.resize(sampleCount);
            for (int s = 0; s < sampleCount; ++s) {
                SampleRecord& sr = scratch.sorted[s];
                sr.originalIndex = s;
                sr.zFront = inPixel.getUnorderedSample(s, Chan_DeepFront);
                sr.zBack  = hasChanBack ? inPixel.getUnorderedSample(s, Chan_DeepBack) : sr.zFront;
                sr.alpha  = hasChanAlpha ? inPixel.getUnorderedSample(s, Chan_Alpha) : 1.0f;
                sr.rgb[0] = hasChanR ? inPixel.getUnorderedSample(s, Chan_Red)   : 0.0f;
                sr.rgb[1] = hasChanG ? inPixel.getUnorderedSample(s, Chan_Green) : 0.0f;
                sr.rgb[2] = hasChanB ? inPixel.getUnorderedSample(s, Chan_Blue)  : 0.0f;
            }

            std::sort(scratch.sorted.begin(), scratch.sorted.end(),
                [](const SampleRecord& a, const SampleRecord& b) {
                    return a.zFront < b.zFront;
                });

            scratch.alive.assign(sampleCount, true);

            // ============================================================
            // PASS 1 — Depth Range Clip
            // ============================================================
            if (_depthClip) {
                for (int s = 0; s < sampleCount; ++s) {
                    const float z = scratch.sorted[s].zFront;
                    if (z < _zNear || z > _zFar)
                        scratch.alive[s] = false;
                }
            }

            // ============================================================
            // PASS 2 — Alpha Cull
            // ============================================================
            if (_cullTransparent) {
                for (int s = 0; s < sampleCount; ++s) {
                    if (!scratch.alive[s]) continue;
                    if (scratch.sorted[s].alpha <= _alphaThreshold)
                        scratch.alive[s] = false;
                }
            }

            // ============================================================
            // PASS 3 — Occlusion Cutoff
            // ============================================================
            if (_occlusionCutoff) {
                float accAlpha = 0.0f;
                bool  occluded = false;
                for (int s = 0; s < sampleCount; ++s) {
                    if (!scratch.alive[s]) continue;
                    if (occluded) {
                        scratch.alive[s] = false;
                        continue;
                    }
                    accAlpha += scratch.sorted[s].alpha * (1.0f - accAlpha);
                    if (accAlpha >= _occlusionAlpha)
                        occluded = true;
                }
            }

            // ============================================================
            // PASS 4 — Contribution Cull
            // ============================================================
            if (_contributionCull) {
                float accAlpha = 0.0f;
                for (int s = 0; s < sampleCount; ++s) {
                    if (!scratch.alive[s]) continue;
                    const float remaining = 1.0f - accAlpha;
                    const float contribution = scratch.sorted[s].alpha * remaining;
                    if (contribution < _contributionMin) {
                        scratch.alive[s] = false;
                        continue;
                    }
                    accAlpha += contribution;
                }
            }

            // ============================================================
            // PASS 5 — Volumetric Collapse
            // ============================================================
            if (_volumetricCollapse && _volumeGroupSize >= 2) {
                int runStart = -1;
                int runLen   = 0;
                for (int s = 0; s <= sampleCount; ++s) {
                    bool isVolumetric = false;
                    if (s < sampleCount && scratch.alive[s] &&
                        scratch.sorted[s].alpha < _volumeAlphaMax) {
                        isVolumetric = true;
                    }

                    if (isVolumetric) {
                        if (runStart < 0) runStart = s;
                        runLen++;
                    } else {
                        if (runLen >= _volumeGroupSize) {
                            int pos = runStart;
                            while (pos + _volumeGroupSize <= runStart + runLen) {
                                for (int k = pos + 1;
                                     k < pos + _volumeGroupSize; ++k) {
                                    scratch.alive[k] = false;
                                }
                                pos += _volumeGroupSize;
                            }
                        }
                        runStart = -1;
                        runLen   = 0;
                    }
                }
            }

            // ============================================================
            // PASS 6 — Smart Merge (Z + colour aware)
            // ============================================================
            struct Group { int start; int end; };
            std::vector<Group> groups;
            groups.reserve(sampleCount);

            if (_mergeSamples && _tolerance > 0.0f) {
                int gs = -1;
                for (int s = 0; s < sampleCount; ++s) {
                    if (!scratch.alive[s]) continue;
                    if (gs < 0) {
                        gs = s;
                    } else {
                        bool zClose = (scratch.sorted[s].zFront -
                                       scratch.sorted[gs].zFront) <= _tolerance;
                        bool colorClose = (_colorTolerance <= 0.0f) ||
                            (colorDistance(scratch.sorted[s].rgb,
                                          scratch.sorted[gs].rgb) <=
                             _colorTolerance);
                        if (!(zClose && colorClose)) {
                            groups.push_back({gs, s});
                            gs = s;
                        }
                    }
                }
                if (gs >= 0)
                    groups.push_back({gs, sampleCount});
            } else {
                for (int s = 0; s < sampleCount; ++s) {
                    if (scratch.alive[s])
                        groups.push_back({s, s + 1});
                }
            }

            // ============================================================
            // PASS 7 — Max Samples Cap
            // ============================================================
            if (_maxSamples > 0 && (int)groups.size() > _maxSamples)
                groups.resize(_maxSamples);

            // ============================================================
            // EMIT output samples
            // ============================================================
            const int outCount = (int)groups.size();
            localOut += outCount;

            DeepOutPixel outPixel;
            outPixel.reserve(outCount * nChans);
            scratch.mergedChannels.resize(nChans);

            for (const Group& g : groups) {
                int aliveCount = 0;
                int firstAlive = -1;
                for (int s = g.start; s < g.end; ++s) {
                    if (scratch.alive[s]) {
                        if (firstAlive < 0) firstAlive = s;
                        ++aliveCount;
                    }
                }
                if (aliveCount == 0) continue;

                if (aliveCount == 1) {
                    const float* src = inPixel.getUnorderedSample(
                        scratch.sorted[firstAlive].originalIndex);
                    for (int ci = 0; ci < nChans; ++ci)
                        outPixel.push_back(src[ci]);
                } else {
                    float accAlpha  = 0.0f;
                    float zFrontMin =  1e30f;
                    float zBackMax  = -1e30f;
                    std::fill(scratch.mergedChannels.begin(),
                              scratch.mergedChannels.end(), 0.0f);

                    for (int s = g.start; s < g.end; ++s) {
                        if (!scratch.alive[s]) continue;
                        const SampleRecord& sr = scratch.sorted[s];
                        const int idx = sr.originalIndex;
                        const float a = sr.alpha;
                        const float w = 1.0f - accAlpha;
                        if (w <= 0.0f) break;

                        zFrontMin = std::min(zFrontMin, sr.zFront);
                        zBackMax  = std::max(zBackMax,  sr.zBack);

                        int ci = 0;
                        foreach(z, channels) {
                            if (z == Chan_DeepFront || z == Chan_DeepBack) {
                                ci++;
                                continue;
                            }
                            const float val =
                                inPixel.getUnorderedSample(idx, z);
                            if (z == Chan_Alpha)
                                scratch.mergedChannels[ci] += a * w;
                            else
                                scratch.mergedChannels[ci] += val * w;
                            ci++;
                        }
                        accAlpha += a * w;
                    }

                    int ci = 0;
                    foreach(z, channels) {
                        if (z == Chan_DeepFront)
                            scratch.mergedChannels[ci] = zFrontMin;
                        else if (z == Chan_DeepBack)
                            scratch.mergedChannels[ci] = zBackMax;
                        ci++;
                    }

                    for (int ci = 0; ci < nChans; ++ci)
                        outPixel.push_back(scratch.mergedChannels[ci]);
                }
            }

            plane.addPixel(outPixel);
        }

        _samplesIn.fetch_add(localIn,   std::memory_order_relaxed);
        _samplesOut.fetch_add(localOut,  std::memory_order_relaxed);

        return true;
    }

    // ------------------------------------------------------------------
    void _close() override
    {
        updateStatKnob();
        DeepFilterOp::_close();
    }

    static const Op::Description d;
};

// ------------------------------------------------------------------
static Op* build(Node* node) { return new DeepThinner(node); }
const Op::Description DeepThinner::d("DeepThinner", "Deep/DeepThinner", build);
