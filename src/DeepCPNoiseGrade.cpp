#include "DDImage/DeepFilterOp.h"
#include "DDImage/DeepSample.h"
#include "DDImage/Knobs.h"
#include "DDImage/RGB.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"
#include "DDImage/Matrix4.h"
#include "DDImage/Row.h"
#include "DDImage/Black.h"

#include "FastNoise.h"

#include <stdio.h>

using namespace DD::Image;


static const char* const shapeNames[] = { "sphere", "cube", 0 };
static const char* const falloffTypeNames[] = { "linear", "smooth", 0 };

// keep mappings for the noise types and names from FastNoise
static const char* const noiseTypeNames[] = {
    "Simplex",
    "Cellular",
    "Cubic",
    "Perlin",
    "Value",
    0
};
static const FastNoise::NoiseType noiseTypes[] = {
    FastNoise::SimplexFractal,
    FastNoise::Cellular,
    FastNoise::Cubic,
    FastNoise::PerlinFractal,
    FastNoise::Value
};

static const char* const fractalTypeNames[] = {
    "FBM",
    "Billow",
    "RigidMulti",
    0
};
static const FastNoise::FractalType fractalTypes[] = {
    FastNoise::FBM,
    FastNoise::Billow,
    FastNoise::RigidMulti,
};

static const char* const distanceFunctionNames[] = {
    "Natural",
    "Euclidean",
    "Manhattan",
    0
};
static const FastNoise::CellularDistanceFunction distanceFunctions[] = {
    FastNoise::Natural,
    FastNoise::Euclidean,
    FastNoise::Manhattan,
};

static const char* const cellularReturnTypeNames[] = {
    "CellValue",
    "Distance",
    "Distance2",
    "Distance2Add",
    "Distance2Sub",
    "Distance2Mul",
    "Distance2Div",
    0
};
static const FastNoise::CellularReturnType cellularReturnTypes[] = {
    FastNoise::CellValue,
    FastNoise::Distance,
    FastNoise::Distance2,
    FastNoise::Distance2Add,
    FastNoise::Distance2Sub,
    FastNoise::Distance2Mul,
    FastNoise::Distance2Div,
};


class DeepCPNoiseGrade : public DeepFilterOp
{
    // values knobs write into go here
    ChannelSet _processChannelSet;
    ChannelSet _auxiliaryChannelSet;
    bool _unpremultRGB;
    bool _unpremultPosition;

    Matrix4 _axisKnob;

    // noise related
    float _frequency;
    int _octaves;
    float _gain;
    float _lacunarity;
    // enum
    int _distanceFunction;
    int _cellularDistanceIndex0;
    int _cellularDistanceIndex1;
    // enum
    int _cellularReturnType;
    // enum
    int _fractalType;
    int _noiseType;
    float _noiseEvolution;
    float _noiseWarp;

    int _shape;
    int _falloffType;
    float _falloff;
    float _falloffGamma;

    float blackpointIn[3];
    float whitepointIn[3];
    float blackIn[3];
    float whiteIn[3];
    float multiplyIn[3];
    float addIn[3];
    float gammaIn[3];

    float blackpointOut[3];
    float whitepointOut[3];
    float blackOut[3];
    float whiteOut[3];
    float multiplyOut[3];
    float addOut[3];
    float gammaOut[3];

    // masking
    Channel _maskChannel;
    Channel _rememberedMaskChannel;
    ChannelSet _maskChannelSet;
    Iop* _maskOp;
    DeepOp* _deepMaskOp;
    bool _doMask;
    bool _deepMask;
    bool _invertMask;
    float _deepMaskTolerance;
    float _mix;

    FastNoise _fastNoise;
    FastNoise _noiseWarpFastNoiseX;
    FastNoise _noiseWarpFastNoiseY;
    FastNoise _noiseWarpFastNoiseZ;

    public:

        DeepCPNoiseGrade(Node* node) : DeepFilterOp(node),
            _processChannelSet(Mask_RGB),
            _auxiliaryChannelSet(Chan_Black),
            _unpremultRGB(true),
            _unpremultPosition(false),
            _noiseType(0),
            _shape(0),
            _falloffType(0),
            _falloff(1.0f),
            _falloffGamma(1.0f),
            _doMask(false),
            _invertMask(false),
            _mix(1.0f)
        {
            // defaults mostly go here
            _axisKnob.makeIdentity();
            _noiseType = 0;
            _noiseEvolution = 0.0f;
            _noiseWarp = 0.0f;

            _fractalType = 0;

            _frequency = .5f;
            _octaves = 5;
            _gain = .5f;
            _lacunarity = 2.0f;
            // cellular
            _cellularReturnType = 0;
            _distanceFunction = 0;
            _cellularDistanceIndex0 = 0;
            _cellularDistanceIndex1 = 1;
            _deepMask = false;
            _deepMaskTolerance = .05;
            for (int i=0; i<3; i++)
            {
                blackpointIn[i] = 0.0f;
                whitepointIn[i] = 1.0f;
                blackIn[i] = 0.0f;
                whiteIn[i] = 1.0f;
                multiplyIn[i] = 1.0f;
                addIn[i] = 0.0f;
                gammaIn[i] = 1.0f;

                blackpointOut[i] = 0.0f;
                whitepointOut[i] = 1.0f;
                blackOut[i] = 0.0f;
                whiteOut[i] = 1.0f;
                multiplyOut[i] = 1.0f;
                addOut[i] = 0.0f;
                gammaOut[i] = 1.0f;
            }
        }

        void _validate(bool);
        bool test_input(int n, Op *op)  const;
        Op* default_input(int input) const;
        const char* input_label(int input, char* buffer) const;
        virtual bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane);
        virtual void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests);
        virtual void knobs(Knob_Callback);
        virtual int knob_changed(DD::Image::Knob* k);

        virtual int minimum_inputs() const { return 2; }
        virtual int maximum_inputs() const { return 2; }
        virtual int optional_input() const { return 1; }
        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};


void DeepCPNoiseGrade::_validate(bool for_real) {
    // make safe the gamma values
    for (int i = 0; i < 3; i++) {
        gammaIn[i] = clamp(gammaIn[i], 0.00001f, 65500.0f);
        gammaOut[i] = clamp(gammaOut[i], 0.00001f, 65500.0f);
    }
    _falloffGamma = clamp(_falloffGamma, 0.00001f, 65500.0f);

    // make safe the falloff
    _falloff = clamp(_falloff, 0.00001f, 1.0f);

    // make safe the mix
    _mix = clamp(_mix, 0.0f, 1.0f);
    
     // Make Noise objects

    _fastNoise.SetNoiseType(noiseTypes[_noiseType]);
    _fastNoise.SetFrequency(_frequency);
    _fastNoise.SetFractalOctaves(_octaves);
    _fastNoise.SetFractalLacunarity(_lacunarity);
    _fastNoise.SetFractalGain(_gain);
    _fastNoise.SetFractalType(fractalTypes[_fractalType]);
    _fastNoise.SetCellularDistanceFunction(distanceFunctions[_distanceFunction]);
    _fastNoise.SetCellularReturnType(cellularReturnTypes[_cellularReturnType]);
    _fastNoise.SetCellularDistance2Indices(
        _cellularDistanceIndex0,
        _cellularDistanceIndex1
    );

    _noiseWarpFastNoiseX.SetNoiseType(FastNoise::Simplex);
    _noiseWarpFastNoiseY.SetNoiseType(FastNoise::Simplex);
    _noiseWarpFastNoiseZ.SetNoiseType(FastNoise::Simplex);

    _maskChannelSet = _maskChannel;

    _maskOp = dynamic_cast<Iop*>(Op::input(1));
    if (_maskOp != NULL)
    {
        std::cout << "_maskOp wasn't NULL, validating\n";
        _maskOp->validate(for_real);
        _doMask = true;
        _deepMask = false;
    } else {
        _deepMaskOp = dynamic_cast<DeepOp*>(Op::input(1));
        if (_deepMaskOp != NULL)
        {
            std::cout << "_deepMaskOp wasn't NULL, validating\n";
            _deepMaskOp->validate(for_real);
            _doMask = true;
            _deepMask = true;
        }
    }

    DeepFilterOp::_validate(for_real);

    DD::Image::ChannelSet outputChannels = _deepInfo.channels();
    outputChannels += _auxiliaryChannelSet;
    outputChannels += _maskChannelSet;
    outputChannels += Chan_DeepBack;
    outputChannels += Chan_DeepFront;
    _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), outputChannels);
}


bool DeepCPNoiseGrade::test_input(int input, Op *op)  const
{
    switch (input)
    {
        case 0:
            return DeepFilterOp::test_input(input, op);
        case 1:
        {
            if (DeepFilterOp::test_input(input, op))
                return true;
            return dynamic_cast<Iop*>(op) != 0;
        }
    }
}


Op* DeepCPNoiseGrade::default_input(int input) const
{
    switch (input)
    {
        case 0:
            return DeepFilterOp::default_input(input);
         case 1:
             Black* dummy;
             return dynamic_cast<Op*>(dummy);
    }
}


const char* DeepCPNoiseGrade::input_label(int input, char* buffer) const
{
    switch (input)
    {
        case 0: return "";
        case 1: return "mask";
    }
}

void DeepCPNoiseGrade::getDeepRequests(Box bbox, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests)
{
    if (!input0())
        return;
    DD::Image::ChannelSet get_channels = channels;
    get_channels += _auxiliaryChannelSet;
    get_channels += _maskChannelSet;
    get_channels += Chan_DeepBack;
    get_channels += Chan_DeepFront;
    requests.push_back(RequestData(input0(), bbox, get_channels, count));
    if (input(1))
    {
        _maskOp = dynamic_cast<Iop*>(Op::input(1));
        if (_maskOp != NULL)
        {
            std::cout << "sending an Iop-style request to input1\n";
            _maskOp->request(bbox, _maskChannelSet, count);
        }
        if (_deepMaskOp != NULL)
        {
            requests.push_back(RequestData(_deepMaskOp, bbox, _maskChannelSet + Mask_Deep, count));
        }
    }
}

bool DeepCPNoiseGrade::doDeepEngine(
    Box bbox,
    const DD::Image::ChannelSet& outputChannels,
    DeepOutputPlane& deep_out_plane
    )
{
    if (!input0())
        return true;

    deep_out_plane = DeepOutputPlane(outputChannels, bbox);

    DD::Image::ChannelSet get_channels = outputChannels;
    get_channels += _auxiliaryChannelSet;

    DeepPlane deepInPlane;
    if (!input0()->deepEngine(bbox, get_channels, deepInPlane))
        return false;

    const DD::Image::ChannelSet inputChannels = input0()->deepInfo().channels();

    const int nOutputChans = outputChannels.size();

    // mask input stuff
    const float* inptr;
    float sideMaskVal;
    int currentYRow;
    DeepPlane deepInMaskPlane;
    Row maskRow(bbox.x(), bbox.r());
    ChannelSet inputMaskChannels;

    if (_maskChannel != Chan_Black)
    {
        if (_deepMask)
        {
            if (_deepMaskOp!=NULL)
            {
                ChannelSet inputMaskChannelsToGet = _maskChannelSet;
                inputMaskChannelsToGet += Chan_DeepBack;
                inputMaskChannelsToGet += Chan_DeepFront;
                _deepMaskOp = dynamic_cast<DeepOp*>(Op::input(1));
                _deepMaskOp->validate(true);
                if (!_deepMaskOp->deepEngine(bbox, inputMaskChannelsToGet, deepInMaskPlane))
                {
                    std::cout << "_deepMaskOp->deepEngine() didn't work\n";
                    return false;
                }
                inputMaskChannels = _deepMaskOp->deepInfo().channels();
            }
        } else
        {
            if (_maskOp!=NULL)
            {
                _maskOp->get(bbox.y(), bbox.x(), bbox.r(), _maskChannelSet, maskRow);
                inptr = maskRow[_maskChannel] + bbox.x();
                currentYRow = bbox.y();
            }
        }
    }


    for (Box::iterator it = bbox.begin(); it != bbox.end(); ++it)
    {
        if (Op::aborted())
            return false; // bail fast on user-interrupt

        // Get the deep pixel from the input plane:
        DeepPixel deepInPixel = deepInPlane.getPixel(it);
        const unsigned nSamples = deepInPixel.getSampleCount();
        if (nSamples == 0) {
            deep_out_plane.addHole(); // no samples, skip it
            continue;
        }

        DeepOutPixel deepOutputPixel;
        deepOutputPixel.reserve(nSamples*nOutputChans);

        // DeepSampleVector deepInMaskPixel;
        unsigned nMaskSamples;
        if (_maskChannel != Chan_Black)
        {
            if (_deepMask)
            {
                if (_deepMaskOp!=NULL)
                {
                    // deepInMaskPixel = deepInMaskPlane.getPixel(it);
                    // nMaskSamples = deepInMaskPixel.getSampleCount();
                }
            } else
            {
                if (_maskOp!=NULL)
                {
                    if (currentYRow != it.y)
                    {
                        _maskOp->get(it.y, bbox.x(), bbox.r(), _maskChannelSet, maskRow);
                        inptr = maskRow[_maskChannel] + bbox.x();
                        sideMaskVal = inptr[it.x];
                        sideMaskVal = clamp(sideMaskVal);
                        if (_invertMask)
                            sideMaskVal = 1.0f - sideMaskVal;
                        currentYRow = it.y;
                    } else
                    {
                        sideMaskVal = inptr[it.x];
                        sideMaskVal = clamp(sideMaskVal);
                        if (_invertMask)
                            sideMaskVal = 1.0f - sideMaskVal;
                    }
                }
            }
        }

        // for each sample
        for (unsigned sampleNo=0; sampleNo < nSamples; ++sampleNo)
        {

            // alpha
            float alpha;
            if (inputChannels.contains(Chan_Alpha))
            {
                alpha = deepInPixel.getUnorderedSample(sampleNo, Chan_Alpha);
            } else
            {
                alpha = 1.0f;
            }

            // generate mask
            float m;
            Vector4 position;
            float distance;
            float noiseValue;
            float positionDisturbance[3];

            if (inputChannels.contains(Chan_DeepFront))
            {
                distance = deepInPixel.getUnorderedSample(sampleNo, Chan_DeepFront);
            } else
            {
                std::cout << "no deep front??\n";
                return false;
            }

            foreach(z, _auxiliaryChannelSet) {
                if (colourIndex(z) >= 3)
                {
                    continue;
                }
                if (inputChannels.contains(z))
                {
                    // grab data from auxiliary channelset
                    if (_unpremultPosition)
                    {
                        position[colourIndex(z)] = deepInPixel.getUnorderedSample(sampleNo, z) / alpha;
                    } else {
                        position[colourIndex(z)] = deepInPixel.getUnorderedSample(sampleNo, z);
                    }
                }
            }
            position[3] = 1.0f;
            Matrix4 inverse__axisKnob = _axisKnob.inverse();
            position = inverse__axisKnob * position;
            position = position.divide_w();

            positionDisturbance[0] = _noiseWarpFastNoiseX.GetSimplex(position[0], position[1], position[2], _noiseEvolution+1000);
            positionDisturbance[1] = _noiseWarpFastNoiseX.GetSimplex(position[0], position[1], position[2], _noiseEvolution+2000);
            positionDisturbance[2] = _noiseWarpFastNoiseX.GetSimplex(position[0], position[1], position[2], _noiseEvolution+3000);

            position[0] += positionDisturbance[0] * _noiseWarp;
            position[1] += positionDisturbance[1] * _noiseWarp;
            position[2] += positionDisturbance[2] * _noiseWarp;

            // TODO: handle this better; fragile now - depends on order of noise list
            if (_noiseType==0)
            {
                // GetNoise(x, y, z, w) only implemented for simplex so far
                noiseValue = _fastNoise.GetNoise(position[0], position[1], position[2], _noiseEvolution);
            } else
            {
                noiseValue = _fastNoise.GetNoise(position[0], position[1], position[2]);
            }
            m = 1.0f - clamp(noiseValue * .5f + .5f, 0.0f, 1.0f);


            m = clamp((1 - m) / _falloff, 0.0f, 1.0f);
            m = pow(m, 1.0f / _falloffGamma);

            // falloff
            if (_falloffType == 0)
            {
                m = clamp(m, 0.0f, 1.0f);
            } else if (_falloffType == 1)
            {
                m = smoothstep(0.0f, 1.0f, m);
            }

            // figure out deep masking
            // this unpotimized double loop is definitely waaaay too slow
            // probably we should insist on inline mask generation
            // and therefore only allow vertical deep masking, no side-input
            // deep masking
            float deepMaskFrontDistance;
            float deepMaskBackDistance;

            if (_maskChannel != Chan_Black)
            {
                if (_deepMask)
                {
                    sideMaskVal = 0.0f;
                    if (_deepMaskOp!=NULL)
                    {
                        DeepPixel deepInMaskPixel = deepInMaskPlane.getPixel(it);
                        nMaskSamples = deepInMaskPixel.getSampleCount();
                        std::cout << "about to do masking loop, nMaskSamples is " << nMaskSamples << "\n";
                        for (unsigned maskSampleNo=0; maskSampleNo < nMaskSamples; ++maskSampleNo)
                        {
                            if (inputMaskChannels.contains(Chan_DeepFront) && inputMaskChannels.contains(Chan_DeepBack))
                            {
                                deepMaskFrontDistance = deepInMaskPixel.getUnorderedSample(maskSampleNo, Chan_DeepFront);
                                deepMaskBackDistance = deepInMaskPixel.getUnorderedSample(maskSampleNo, Chan_DeepBack);
                                deepMaskFrontDistance -= _deepMaskTolerance;
                                deepMaskBackDistance += _deepMaskTolerance;

                                std::cout << "distance - deepMaskFrontDistance " << distance - deepMaskFrontDistance << "\n";
                                if (distance >= deepMaskFrontDistance && distance <= deepMaskBackDistance)
                                {
                                    // hit, do masking
                                    std::cout << "hit!\n";
                                    sideMaskVal += deepInMaskPixel.getUnorderedSample(maskSampleNo, _maskChannel);
                                }
                            } else
                            {
                                std::cout << "inputMaskChannels.contains() was false\n";
                                sideMaskVal += 0.0f;
                            }
                        }
                        sideMaskVal = clamp(sideMaskVal);
                    }
                }
            }

            // process the sample
            float input_val;
            float inside_val;
            float outside_val;
            float output_val;
            // for each channel
            foreach(z, outputChannels)
            {
                // channels we know we should ignore
                if (
                       z == Chan_Z
                    || z == Chan_Alpha
                    || z == Chan_DeepFront
                    || z == Chan_DeepBack
                    )
                {
                    deepOutputPixel.push_back(deepInPixel.getUnorderedSample(sampleNo, z));
                    continue;
                }

                // bail if mix is 0
                if (_mix == 0.0f)
                {
                    deepOutputPixel.push_back(deepInPixel.getUnorderedSample(sampleNo, z));
                    continue;
                }


                // only operate on rgb, i.e. first three channels
                if (colourIndex(z) >= 3)
                {
                    deepOutputPixel.push_back(deepInPixel.getUnorderedSample(sampleNo, z));
                    continue;
                }

                // do the thing, zhu-lee
                input_val = deepInPixel.getUnorderedSample(sampleNo, z);
                if (_unpremultRGB)
                    input_val /= alpha;

                // val * multiply * (gain - lift) / (whitepoint - blackpoint) + offset + lift - (multiply * (gain - lift) / (whitepoint - blackpoint)) * blackpoint, 1/gamma
                // i believe this is similar or identical to what nuke uses in a grade node, so should be familiar
                // despite not really being reasonable
                inside_val= pow(input_val * multiplyIn[colourIndex(z)] * (whiteIn[colourIndex(z)] - blackIn[colourIndex(z)]) / (whitepointIn[colourIndex(z)] - blackpointIn[colourIndex(z)]) + addIn[colourIndex(z)] + blackIn[colourIndex(z)] - (multiplyIn[colourIndex(z)] * (whiteIn[colourIndex(z)] - blackIn[colourIndex(z)]) / (whitepointIn[colourIndex(z)] - blackpointIn[colourIndex(z)])) * blackpointIn[colourIndex(z)], 1.0f/gammaIn[colourIndex(z)]);

                outside_val= pow(input_val * multiplyOut[colourIndex(z)] * (whiteOut[colourIndex(z)] - blackOut[colourIndex(z)]) / (whitepointOut[colourIndex(z)] - blackpointOut[colourIndex(z)]) + addOut[colourIndex(z)] + blackOut[colourIndex(z)] - (multiplyOut[colourIndex(z)] * (whiteOut[colourIndex(z)] - blackOut[colourIndex(z)]) / (whitepointOut[colourIndex(z)] - blackpointOut[colourIndex(z)])) * blackpointOut[colourIndex(z)], 1.0f/gammaOut[colourIndex(z)]);

                // apply mask
                output_val = (inside_val * m + outside_val * (1 - m));

                if (_unpremultRGB)
                {
                    output_val = output_val * alpha;
                } // need do nothing if we're not required to premult

                if (_mix == 1.0f)
                {
                    if (_maskChannel != Chan_Black)
                    {
                        if (_maskOp != NULL || _deepMaskOp != NULL)
                            output_val = output_val * sideMaskVal + input_val * (1.0f - sideMaskVal);
                    }
                    deepOutputPixel.push_back(output_val);
                } else {
                    output_val = output_val * _mix + input_val * (1.0f - _mix);
                    if (_maskChannel != Chan_Black)
                    {
                        if (_maskOp != NULL || _deepMaskOp != NULL)
                            output_val = output_val * sideMaskVal + input_val * (1.0f - sideMaskVal);
                    }
                    deepOutputPixel.push_back(output_val);
                } // we checked for mix == 0 earlier
            }
        }
        // Add to output plane:
        deep_out_plane.addPixel(deepOutputPixel);
    }
    return true;
}

void DeepCPNoiseGrade::knobs(Knob_Callback f)
{
    Input_ChannelSet_knob(f, &_processChannelSet, 0, "channels");
    Input_ChannelSet_knob(f, &_auxiliaryChannelSet, 0, "position_data");
    SetFlags(f, Knob::NO_ALPHA_PULLDOWN);
    Bool_knob(f, &_unpremultRGB, "unpremult_rgb", "(Un)premult RGB");
    SetFlags(f, Knob::STARTLINE);
    Tooltip(f, "Unpremultiply channels in 'channels' before grading, "
    "then premult again after. This is always by rgba.alpha, as this "
    "is how Deep data is constructed. Should probably always be checked.");
    Bool_knob(f, &_unpremultPosition, "unpremult_position_data", "Unpremult Position Data");
    Tooltip(f, "Uncheck for ScanlineRender Deep data, check for (probably) "
    "all other renderers. Nuke stores position data from the ScanlineRender "
    "node unpremultiplied, contrary to the Deep spec. Other renderers "
    "presumably store position data (and all other data) premultiplied, "
    "as required by the Deep spec.");


    Divider(f);
    BeginGroup(f, "Position");
    Axis_knob(f, &_axisKnob, "selection");
    EndGroup(f); // Position
    BeginClosedGroup(f, "Noise");
    Enumeration_knob(f, &_noiseType, noiseTypeNames, "noiseType");
    Tooltip(f,
        "Simplex: 4D noise - aka improved perlin"
        "Cellular: 3D noise - aka Voronoi"
        "Cubic: 3D noise - more random than Perlin?"
        "Perlin: 3D noise - the classic"
        "Value: 3D noise - sort of square and blocky"
    );
    Float_knob(f, &_falloff, "falloff");
    Float_knob(f, &_falloffGamma, "falloff_gamma");
    Float_knob(f, &_noiseWarp, "noise_warp_strength");
    Tooltip(f,
        "Mult the position data by a simplex noise"
        "before generating the main noise, creating"
        "swirly patterns."
    );
    Float_knob(f, &_noiseEvolution, "noise_evolution");
    Tooltip(f,
        "For Simplex noise (the only 4D noise), this smoothly"
        "changes the noise in a non-spatial dimension ("
        "like evolution over time...)."
    );
    // fractal functions
    BeginClosedGroup(f, "Fractal");
    Enumeration_knob(f, &_fractalType, fractalTypeNames, "fractal_type");
    Float_knob(f, &_frequency, "frequency");
    Tooltip(f,
    "Affects how coarse the noise output is."
    "Used in all noise generation except White Noise."
    );
    Int_knob(f, &_octaves, "fractal_octaves");
    Tooltip(f,
        "The amount of noise layers used to create the fractal."
        "Used in all fractal noise generation."
    );
    Float_knob(f, &_lacunarity, "lacunarity");
    Tooltip(f,
        "The frequency multiplier between each octave."
    );
    Float_knob(f, &_gain, "fractal_gain");
    Tooltip(f,
        "The relative strength of noise from each layer"
        "when compared to the last one."
    );
    EndGroup(f); // Fractal
    // cellular functions
    BeginClosedGroup(f, "Cellular");
    Enumeration_knob(f, &_distanceFunction, distanceFunctionNames, "distance_function");
    Tooltip(f,
        "The distance function used to calculate the cell"
        "for a given point."
    );
    Enumeration_knob(f, &_cellularReturnType, cellularReturnTypeNames, "cellular_type");
    Int_knob(f, &_cellularDistanceIndex0, "cellular_distance_index0");
    Int_knob(f, &_cellularDistanceIndex1, "cellular_distance_index1");
    EndGroup(f); // Cellular
    // EndGroup Noise
    EndGroup(f);

    /*
    Divider(f, "Shape");
    Enumeration_knob(f, &_shape, shapeNames, "shape");
    Enumeration_knob(f, &_falloffType, falloffTypeNames, "falloffType");
    */

    Divider(f, "");
    BeginClosedGroup(f, "Inside Mask Grades");

    Color_knob(f, blackpointIn, IRange(-1, 1), "blackpoint_inside", "blackpoint");
    Tooltip(f, "This color is turned into black");
    Color_knob(f, whitepointIn, IRange(0, 4), "whitepoint_inside", "whitepoint");
    Tooltip(f, "This color is turned into white");
    Color_knob(f, blackIn, IRange(-1, 1), "black_inside", "lift");
    Tooltip(f, "Black is turned into this color");
    Color_knob(f, whiteIn, IRange(0, 4), "white_inside", "gain");
    Tooltip(f, "White is turned into this color");
    Color_knob(f, multiplyIn, IRange(0, 4), "multiply_inside", "multiply");
    Tooltip(f, "Constant to multiply result by");
    Color_knob(f, addIn, IRange(-1, 1), "add_inside", "offset");
    Tooltip(f, "Constant to add to result (raises both black & white, unlike lift)");
    Color_knob(f, gammaIn, IRange(0.2, 5), "gamma_inside", "gamma");
    Tooltip(f, "Gamma correction applied to final result");

    EndGroup(f);

    BeginClosedGroup(f, "Outside Mask Grades");

    Color_knob(f, blackpointOut, IRange(-1, 1), "blackpoint_outside", "blackpoint");
    Tooltip(f, "This color is turned into black");
    Color_knob(f, whitepointOut, IRange(0, 4), "whitepoint_outside", "whitepoint");
    Tooltip(f, "This color is turned into white");
    Color_knob(f, blackOut, IRange(-1, 1), "black_outside", "lift");
    Tooltip(f, "Black is turned into this color");
    Color_knob(f, whiteOut, IRange(0, 4), "white_outside", "gain");
    Tooltip(f, "White is turned into this color");
    Color_knob(f, multiplyOut, IRange(0, 4), "multiply_outside", "multiply");
    Tooltip(f, "Constant to multiply result by");
    Color_knob(f, addOut, IRange(-1, 1), "add_outside", "offset");
    Tooltip(f, "Constant to add to result (raises both black & white, unlike lift)");
    Color_knob(f, gammaOut, IRange(0.2, 5), "gamma_outside", "gamma");
    Tooltip(f, "Gamma correction applied to final result");

    EndGroup(f);
    Divider(f, "");

    Input_Channel_knob(f, &_maskChannel, 1, 1, "mask");
    Bool_knob(f, &_invertMask, "invert_mask", "invert");
    Float_knob(f, &_deepMaskTolerance, "deep_mask_tolerance");
    Float_knob(f, &_mix, "mix");
}

int DeepCPNoiseGrade::knob_changed(DD::Image::Knob* k)
{
    if (k->is("inputChange"))
    {
        if (!input(1))
        {
            _rememberedMaskChannel = _maskChannel;
            knob("mask")->set_value(Chan_Black);
        } else
        {
            knob("mask")->set_value(_rememberedMaskChannel);
        }
        return 1;
    }

    return DeepFilterOp::knob_changed(k);
}

const char* DeepCPNoiseGrade::node_help() const
{
    return
    "Mask a grade op by a world-position pass.";
}

static Op* build(Node* node) { return new DeepCPNoiseGrade(node); }
const Op::Description DeepCPNoiseGrade::d("DeepCPNoiseGrade", 0, build);
