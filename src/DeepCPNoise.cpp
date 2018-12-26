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


class DeepCPNoise : public DeepFilterOp
{
    // values knobs write into go here
    ChannelSet _outputChannelSet;
    ChannelSet _auxiliaryChannelSet;
    bool _premultOutput;
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

    float blackpoint[3];
    float whitepoint[3];
    float black[3];
    float white[3];
    float multiply[3];
    float add[3];
    float gamma[3];

    // precalculate grade coefficients
    float A[3];
    float B[3];
    float G[3];

    //
    bool _reverse;
    bool _blackClamp;
    bool _whiteClamp;

    // masking
    Channel _deepMaskChannel;
    ChannelSet _deepMaskChannelSet;
    bool _doDeepMask;
    bool _invertDeepMask;
    bool _unpremultDeepMask;

    Channel _sideMaskChannel;
    Channel _rememberedMaskChannel;
    ChannelSet _sideMaskChannelSet;
    Iop* _maskOp;
    bool _doSideMask;
    bool _invertSideMask;
    float _mix;

    FastNoise _fastNoise;
    FastNoise _noiseWarpFastNoiseX;
    FastNoise _noiseWarpFastNoiseY;
    FastNoise _noiseWarpFastNoiseZ;

    public:

        DeepCPNoise(Node* node) : DeepFilterOp(node),
            _outputChannelSet(Mask_RGB),
            _auxiliaryChannelSet(Chan_Black),
            _deepMaskChannel(Chan_Black),
            _deepMaskChannelSet(Chan_Black),
            _rememberedMaskChannel(Chan_Black),
            _sideMaskChannel(Chan_Black),
            _sideMaskChannelSet(Chan_Black),
            _doDeepMask(false),
            _invertDeepMask(false),
            _unpremultDeepMask(true),
            _premultOutput(true),
            _unpremultPosition(true),
            _noiseType(0),
            _doSideMask(false),
            _invertSideMask(false),
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
            for (int i=0; i<3; i++)
            {
                blackpoint[i] = 0.0f;
                whitepoint[i] = 1.0f;
                black[i] = 0.0f;
                white[i] = 1.0f;
                multiply[i] = 1.0f;
                add[i] = 0.0f;
                gamma[i] = 1.0f;
            }
        }

        void _validate(bool);
        virtual bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane);
        virtual void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests);
        virtual void knobs(Knob_Callback);
        virtual int knob_changed(DD::Image::Knob* k);

        bool test_input(int n, Op *op)  const;
        Op* default_input(int input) const;
        const char* input_label(int input, char* buffer) const;
        virtual int minimum_inputs() const { return 2; }
        virtual int maximum_inputs() const { return 2; }
        virtual int optional_input() const { return 1; }
        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};


void DeepCPNoise::_validate(bool for_real)
{

    for (int i = 0; i < 3; i++) {
        // make safe the gamma values
        gamma[i] = clamp(gamma[i], 0.00001f, 65500.0f);

        // for calculating the grade - precompute the coefficients
        A[i] = multiply[i] * ((white[i] - black[i]) / (whitepoint[i] - blackpoint[i]));
        B[i] = add[i] + black[i] - A[i] * blackpoint[i];
        G[i] = 1.0f / gamma[i];
        if (_reverse)
        {
            // opposite linear ramp
            if (A[i])
            {
                A[i] = 1.0f / A[i];
            } else
            {
                A[i] = 1.0f;
            }
            B[i] = -B[i] * A[i];
            // inverse gamma
            G[i] = 1.0f/G[i];
        }
    }


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

    _sideMaskChannelSet = _sideMaskChannel;

    _maskOp = dynamic_cast<Iop*>(Op::input(1));
    if (_maskOp != NULL && _sideMaskChannel != Chan_Black)
    {
        _maskOp->validate(for_real);
        _doSideMask = true;
    } else {
        _doSideMask = false;
    }

    if (_deepMaskChannel != Chan_Black)
    { _doDeepMask = true; }
    else
    { _doDeepMask = false; }

    DeepFilterOp::_validate(for_real);

    // add our output channels to the _deepInfo
    ChannelSet new_channelset;
    new_channelset = _deepInfo.channels();
    new_channelset += _outputChannelSet;
    _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), new_channelset);
}


void DeepCPNoise::getDeepRequests(Box bbox, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests)
{
    if (!input0())
        return;

    // make sure we're asking for all required channels
    ChannelSet get_channels = channels;
    get_channels += _auxiliaryChannelSet;
    get_channels += _deepMaskChannel;
    get_channels += Chan_DeepBack;
    get_channels += Chan_DeepFront;
    requests.push_back(RequestData(input0(), bbox, get_channels, count));

    // make sure to request anything we want from the Iop side, too
    if (_doSideMask)
        _maskOp->request(bbox, _sideMaskChannelSet, count);
}

bool DeepCPNoise::doDeepEngine(
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
    get_channels += _deepMaskChannel;

    DeepPlane deepInPlane;
    if (!input0()->deepEngine(bbox, get_channels, deepInPlane))
        return false;

    ChannelSet available;
    available = deepInPlane.channels();

    const DD::Image::ChannelSet inputChannels = input0()->deepInfo().channels();

    const int nOutputChans = outputChannels.size();

    // mask input stuff
    float sideMaskVal;
    int currentYRow;
    Row maskRow(bbox.x(), bbox.r());

    if (_doSideMask)
    {
        _maskOp->get(bbox.y(), bbox.x(), bbox.r(), _sideMaskChannelSet, maskRow);
        currentYRow = bbox.y();
    }

    float result;

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

        // create initialized, don't create and then init
        size_t outPixelSize;
        outPixelSize = nSamples * nOutputChans * sizeof(float);
        DeepOutPixel deepOutputPixel(outPixelSize);

        if (_doSideMask)
        {
            if (currentYRow != it.y)
            {
                // we have not already gotten this row, get it now
                _maskOp->get(it.y, bbox.x(), bbox.r(), _sideMaskChannelSet, maskRow);
                currentYRow = it.y;
            }
            sideMaskVal = maskRow[_sideMaskChannel][it.x];
            sideMaskVal = clamp(sideMaskVal);
            if (_invertSideMask)
                sideMaskVal = 1.0f - sideMaskVal;
        }

        // for each sample
        for (unsigned sampleNo=0; sampleNo < nSamples; ++sampleNo)
        {

            // alpha
            float alpha;
            if (available.contains(Chan_Alpha))
            {
                alpha = deepInPixel.getUnorderedSample(sampleNo, Chan_Alpha);
            } else
            {
                alpha = 1.0f;
            }


            // deep masking
            float deepMaskVal;
            if (_doDeepMask)
            {
                if (available.contains(_deepMaskChannel))
                {
                    deepMaskVal = deepInPixel.getUnorderedSample(sampleNo, _deepMaskChannel);
                    if (_unpremultDeepMask)
                    {
                        if (alpha != 0.0f)
                        {
                            deepMaskVal /= alpha;
                        } else
                        {
                            deepMaskVal = 0.0f;
                        }
                    }
                    if (_invertDeepMask)
                        deepMaskVal = 1.0f - deepMaskVal;
                } else
                {
                    deepMaskVal = 1.0f;
                }
            }

            // generate noise
            float m;
            Vector4 position;
            float noiseValue;
            float positionDisturbance[3];

            foreach(z, _auxiliaryChannelSet) {
                if (colourIndex(z) >= 3)
                {
                    continue;
                }
                if (available.contains(z))
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

            // process the sample
            float input_val;
            float inside_val;
            float outside_val;
            float output_val;
            int cIndex;
            // for each channel
            foreach(z, outputChannels)
            {
                cIndex = colourIndex(z);
                // passthrough channels we know we should ignore
                // passthrough all but the output channels
                // only operate on rgb, i.e. first three channels
                // bail if mix is 0
                if (
                       z == Chan_Z
                    || z == Chan_Alpha
                    || z == Chan_DeepFront
                    || z == Chan_DeepBack
                    || !_outputChannelSet.contains(z)
                    || _mix == 0.0f
                    )
                {
                    deepOutputPixel.push_back(deepInPixel.getUnorderedSample(sampleNo, z));
                    continue;
                }

                input_val = m;
                output_val = m;

                // grade the noise; i *think* it will be more efficient to
                // do it here, than in a separate op (with all associated)
                // copying and whatnot

                if (_reverse)
                {
                    // opposite gamma, precomputed
                    if (G[cIndex] != 1.0f)
                        input_val = pow(input_val, G[cIndex]);
                    // then inverse linear ramp we have already precomputed
                    output_val = A[cIndex] * input_val + B[cIndex];
                } else
                {
                    output_val = A[cIndex] * input_val + B[cIndex];
                    if (G[cIndex] != 1.0f)
                        output_val = pow(output_val, G[cIndex]);
                }

                if (_blackClamp)
                    output_val = MAX(output_val, 0.0f);

                if (_whiteClamp)
                    output_val = MIN(output_val, 1.0f);

                // we checked for mix == 0 earlier, only need to handle non-1
                if (_mix < 1.0f)
                    output_val *= _mix;

                float mask = 1.0f;

                if (_doSideMask)
                    mask *= sideMaskVal;

                if (_doDeepMask)
                    mask *= deepMaskVal;

                if (_doDeepMask || _doSideMask)
                    output_val = output_val * mask;

                if (_premultOutput)
                    output_val *= alpha;

                deepOutputPixel.push_back(output_val);
            }
        }
        // Add to output plane:
        deep_out_plane.addPixel(deepOutputPixel);
    }
    return true;
}

void DeepCPNoise::knobs(Knob_Callback f)
{
    Input_ChannelSet_knob(f, &_auxiliaryChannelSet, 0, "position_data");
    Bool_knob(f, &_unpremultPosition, "unpremult_position_data", "unpremult position");
    Tooltip(f, "Uncheck for ScanlineRender Deep data, check for (probably) "
    "all other renderers. Nuke stores position data from the ScanlineRender "
    "node unpremultiplied, contrary to the Deep spec. Other renderers "
    "presumably store position data (and all other data) premultiplied, "
    "as required by the Deep spec.");
    Input_ChannelSet_knob(f, &_outputChannelSet, 0, "output");
    Bool_knob(f, &_premultOutput, "premult_output", "premult output");
    Tooltip(f, "If, for some reason, you want your mask stored without "
    "premultipling it, contrary to the Deep spec, uncheck this. "
    "Should probably always be checked.");


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

    Divider(f, "");
    BeginClosedGroup(f, "Noise Grade");

    Color_knob(f, blackpoint, IRange(-1, 1), "blackpoint", "blackpoint");
    Tooltip(f, "This color is turned into black");
    Color_knob(f, whitepoint, IRange(0, 4), "whitepoint", "whitepoint");
    Tooltip(f, "This color is turned into white");
    Color_knob(f, black, IRange(-1, 1), "black", "lift");
    Tooltip(f, "Black is turned into this color");
    Color_knob(f, white, IRange(0, 4), "white", "gain");
    Tooltip(f, "White is turned into this color");
    Color_knob(f, multiply, IRange(0, 4), "multiply", "multiply");
    Tooltip(f, "Constant to multiply result by");
    Color_knob(f, add, IRange(-1, 1), "add", "offset");
    Tooltip(f, "Constant to add to result (raises both black & white, unlike lift)");
    Color_knob(f, gamma, IRange(0.2, 5), "gamma", "gamma");
    Tooltip(f, "Gamma correction applied to final result");
    Bool_knob(f, &_reverse, "reverse");
    SetFlags(f, Knob::STARTLINE);
    Bool_knob(f, &_blackClamp, "black_clamp");
    Bool_knob(f, &_whiteClamp, "white_clamp");


    EndGroup(f);

    Divider(f, "");

    Input_Channel_knob(f, &_deepMaskChannel, 1, 0, "deep_mask", "deep input mask");
    Bool_knob(f, &_invertDeepMask, "invert_deep_mask", "invert");
    Bool_knob(f, &_unpremultDeepMask, "unpremult_deep_mask", "unpremult");

    Input_Channel_knob(f, &_sideMaskChannel, 1, 1, "side_mask", "side input mask");
    Bool_knob(f, &_invertSideMask, "invert_mask", "invert");
    Float_knob(f, &_mix, "mix");
}

int DeepCPNoise::knob_changed(DD::Image::Knob* k)
{
    if (k->is("inputChange"))
    {
        // test input 1
        bool input1_connected = dynamic_cast<Iop*>(input(1)) != 0;
        if (!input1_connected)
        {
            _rememberedMaskChannel = _sideMaskChannel;
            knob("side_mask")->set_value(Chan_Black);
        } else
        {
            if (_rememberedMaskChannel == Chan_Black)
                { knob("side_mask")->set_value(Chan_Alpha); }
            else
                { knob("side_mask")->set_value(_rememberedMaskChannel); }
        }
        return 1;
    }

    return DeepFilterOp::knob_changed(k);
}


bool DeepCPNoise::test_input(int input, Op *op)  const
{
    switch (input)
    {
        case 0:
            return DeepFilterOp::test_input(input, op);
        case 1:
            return dynamic_cast<Iop*>(op) != 0;
    }
}


Op* DeepCPNoise::default_input(int input) const
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


const char* DeepCPNoise::input_label(int input, char* buffer) const
{
    switch (input)
    {
        case 0: return "";
        case 1: return "mask";
    }
}


const char* DeepCPNoise::node_help() const
{
    return
    "Generate noise to use for grading operations.";
}

static Op* build(Node* node) { return new DeepCPNoise(node); }
const Op::Description DeepCPNoise::d("DeepCPNoise", 0, build);
