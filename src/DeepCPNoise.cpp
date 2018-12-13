#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/RGB.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"
#include "DDImage/Matrix4.h"

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
    ChannelSet process_channelset;
    ChannelSet wp_channelset;
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

    float _mix;

    FastNoise _fastNoise;
    FastNoise _noiseWarpFastNoiseX;
    FastNoise _noiseWarpFastNoiseY;
    FastNoise _noiseWarpFastNoiseZ;

    public:

        DeepCPNoise(Node* node) : DeepFilterOp(node),
            process_channelset(Mask_RGB),
            wp_channelset(Chan_Black),
            _unpremultRGB(true),
            _unpremultPosition(false),
            _noiseType(0),
            _shape(0),
            _falloffType(0),
            _falloff(1.0f),
            _falloffGamma(1.0f),
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


        virtual bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane);


        // Wrapper function to work around the "non-virtual thunk" issue on linux when symbol hiding is enabled.
        virtual void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests)
        {
             DeepFilterOp::getDeepRequests(box, channels, count, requests);
        }


        // We always need the rgb no matter what channels are asked for:
        void in_channels(int, ChannelSet& channels) const
        {
            if (channels & Mask_RGB)
                channels += Mask_RGB;
            channels += process_channelset;
            channels += wp_channelset;
            channels += Chan_Alpha;
        }

        // need to understand these better
        static const Iop::Description d;
        const char* Class() const { return d.name; }


        // for menu listing and function controls
        virtual void knobs(Knob_Callback);



        virtual Op* op()
        {
            return this;
        }

        void _validate(bool);

        const char* node_help() const;
};


// Safe power function that does something reasonable for negative numbers
// e must be clamped to the range -MAXPOW..MAXPOW
// Retained as an option to replace normal power function later if needed
#define MAXPOW 1000.0f
static inline float P(float x, float e)
{
    return x > 0 ? powf(x, e) : x;
}

void DeepCPNoise::_validate(bool for_real)
{
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

    // TODO: should probably check into whether we're using an outside
    //       grade, and if not, we can optimize by not processing any
    //       pixel with m == 0.0f;

    // TODO: should check for a few degenerate states like mix=0 and
    //       optimally pass through in those cases

     DeepFilterOp::_validate(for_real);

    ChannelSet new_channelset;
    new_channelset = _deepInfo.channels();
    new_channelset += wp_channelset;
    _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), new_channelset);
}

bool DeepCPNoise::doDeepEngine(DD::Image::Box box, const ChannelSet& channels, DeepOutputPlane& outPlane)
{
    if (!input0())
	   return true;

    DeepOp* in = input0();
    DeepPlane inPlane;

    ChannelSet needed = channels;
    needed += Mask_DeepFront;
    needed += wp_channelset;

    if (!in->deepEngine(box, needed, inPlane))
	   return false;

    outPlane = DeepOutputPlane(needed, box);

    for (DD::Image::Box::iterator it = box.begin(); it != box.end(); it++)
	{

		const int x = it.x;
		const int y = it.y;

		DeepPixel inPixel = inPlane.getPixel(it);

		DeepOutPixel outPixel(inPixel.getSampleCount() * channels.size());

		for (size_t sampleNo = 0; sampleNo < inPixel.getSampleCount(); sampleNo++)
		{

			 // float z = inPixel.getUnorderedSample(sample, DD::Image::Chan_DeepFront);

		    // alpha
		    float alpha;
		    alpha = inPixel.getUnorderedSample(sampleNo, Chan_Alpha);

		    // generate mask
		    float m;
		    Vector4 position;
		    float distance;

            // wp_channelset &= channels;
            position[0] = 0.0f;
            position[1] = 0.0f;
            position[2] = 0.0f;
            position[3] = 0.0f;
		    foreach(z, wp_channelset) {
		        if (colourIndex(z) >= 3)
		        {
                    std::cout << "bailing, colour index is " << colourIndex(z) << "\n";
		            continue;
		        }
		        if (z == Chan_Unused)
		        {
		            std::cout << "Chan_Unused in wp_channelset\n";
		            position[colourIndex(z)] = 0.0f;
		            continue;
		        }
		        if (_unpremultPosition)
		        {
		            position[colourIndex(z)] = inPixel.getUnorderedSample(sampleNo, z) / alpha;
                    std::cout << "unpremult position at " << position[0] << ", " << position[1] << ", " << position[2] << "\n";
		        } else {
		            position[colourIndex(z)] = inPixel.getUnorderedSample(sampleNo, z);
                    std::cout << "position at " << position[0] << ", " << position[1] << ", " << position[2] << "\n";
		        }
		    }
		    position[3] = 1.0f;
		    Matrix4 inverse__axisKnob = _axisKnob.inverse();
		    position = inverse__axisKnob * position;
		    position = position.divide_w();

		    if (_shape == 0)
		    {
		        // sphere
		        distance = pow(position[0] * position[0] + position[1] * position[1] + position[2] * position[2], .5);
		    } else
		    {
		        // cube
		        distance = clamp(1-abs(position[0])) * clamp(1-abs(position[1])) * clamp(1-abs(position[2]));
		    }


		    float noiseValue;
		    float positionDisturbance[3];
		    positionDisturbance[0] = _noiseWarpFastNoiseX.GetSimplex(position[0], position[1], position[2], _noiseEvolution+1000);
		    positionDisturbance[1] = _noiseWarpFastNoiseX.GetSimplex(position[0], position[1], position[2], _noiseEvolution+2000);
		    positionDisturbance[2] = _noiseWarpFastNoiseX.GetSimplex(position[0], position[1], position[2], _noiseEvolution+3000);

		    position[0] += positionDisturbance[0] * _noiseWarp;
		    position[1] += positionDisturbance[1] * _noiseWarp;
		    position[2] += positionDisturbance[2] * _noiseWarp;

		    if (_noiseType==2)
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

		    // process the sample
		    float input_val;
		    float inside_val;
		    float outside_val;
		    float outPixel_val;

		    foreach(z, channels)
            {
		        // channels we know we should ignore
		        if (
		                z == Chan_Z
		            || z == Chan_Alpha
		            || z == Chan_DeepFront
		            || z == Chan_DeepBack
		            )
		        {
		            outPixel.push_back(inPixel.getUnorderedSample(sampleNo, z));
		            continue;
		        }

		        // bail if mix is 0
		        if (_mix == 0.0f)
		        {
		            outPixel.push_back(inPixel.getUnorderedSample(sampleNo, z));
		            continue;
		        }

		        // is this the best way to restrict the processed channelset? maybe...
		        if (process_channelset & z)
		        {
		            // only operate on rgb, i.e. first three channels
		            if (colourIndex(z) >= 3)
		            {
		                outPixel.push_back(inPixel.getUnorderedSample(sampleNo, z));
		                continue;
		            }

		            // do the thing, zhu-lee
		            if (_unpremultRGB)
		            {
		                input_val = inPixel.getUnorderedSample(sampleNo, z) / alpha;
		            } else {
		                input_val = inPixel.getUnorderedSample(sampleNo, z);
		            }

		            // val * multiply * (gain - lift) / (whitepoint - blackpoint) + offset + lift - (multiply * (gain - lift) / (whitepoint - blackpoint)) * blackpoint, 1/gamma
		            // i believe this is similar or identical to what nuke uses in a grade node, so should be familiar
		            // despite not really being reasonable
		            inside_val= pow(input_val * multiplyIn[colourIndex(z)] * (whiteIn[colourIndex(z)] - blackIn[colourIndex(z)]) / (whitepointIn[colourIndex(z)] - blackpointIn[colourIndex(z)]) + addIn[colourIndex(z)] + blackIn[colourIndex(z)] - (multiplyIn[colourIndex(z)] * (whiteIn[colourIndex(z)] - blackIn[colourIndex(z)]) / (whitepointIn[colourIndex(z)] - blackpointIn[colourIndex(z)])) * blackpointIn[colourIndex(z)], 1.0f/gammaIn[colourIndex(z)]);

		            outside_val= pow(input_val * multiplyOut[colourIndex(z)] * (whiteOut[colourIndex(z)] - blackOut[colourIndex(z)]) / (whitepointOut[colourIndex(z)] - blackpointOut[colourIndex(z)]) + addOut[colourIndex(z)] + blackOut[colourIndex(z)] - (multiplyOut[colourIndex(z)] * (whiteOut[colourIndex(z)] - blackOut[colourIndex(z)]) / (whitepointOut[colourIndex(z)] - blackpointOut[colourIndex(z)])) * blackpointOut[colourIndex(z)], 1.0f/gammaOut[colourIndex(z)]);

		            // apply mask
		            outPixel_val = (inside_val * m + outside_val * (1 - m));

		            if (_unpremultRGB)
		            {
		                outPixel_val = outPixel_val * alpha;
		            } // need do nothing if we're not required to premult

		            if (_mix == 1.0f)
		            {
		                outPixel.push_back(outPixel_val);
		            } else {
		                outPixel_val = outPixel_val * _mix + input_val * (1 - _mix);
		            } // we checked for mix == 0 earlier
		        } else {
		            outPixel.push_back(inPixel.getUnorderedSample(sampleNo, z));
		            continue;
		        }
        	}
		}

		outPlane.addPixel(outPixel);
    }

    return true;
}


void DeepCPNoise::knobs(Knob_Callback f)
{
    InputOnly_ChannelSet_knob(f, &process_channelset, 0, "channels");
    InputOnly_ChannelSet_knob(f, &wp_channelset, 0, "position_data");
    SetFlags(f, Knob::NO_ALPHA_PULLDOWN);
    Bool_knob(f, &_unpremultRGB, "unpremult_rgb", "(Un)premult RGB");
    SetFlags(f, Knob::STARTLINE);
    Tooltip(f, "Unpremultiply channels in 'channels' before grading, \n"
    "then premult again after. This is always by rgba.alpha, as this \n"
    "is how Deep data is constructed. Should probably always be checked.");
    Bool_knob(f, &_unpremultPosition, "unpremult_position_data", "Unpremult Position Data");
    Tooltip(f, "Uncheck for ScanlineRender Deep data, check for (probably) \n"
    "all other renderers. Nuke stores position data from the ScanlineRender \n"
    "node unpremultiplied, contrary to the Deep spec. Other renderers \n"
    "presumably store position data (and all other data) premultiplied, \n"
    "as required by the Deep spec.");


    Divider(f);
    BeginGroup(f, "Position");
    Axis_knob(f, &_axisKnob, "selection");
    EndGroup(f); // Position
    BeginClosedGroup(f, "Noise");
    Enumeration_knob(f, &_noiseType, noiseTypeNames, "noiseType");
    Tooltip(f,
        "Value: 3D noise - sort of square and blocky\n"
        "Perlin: 3D noise - the classic\n"
        "Simplex: 4D noise - aka improved perlin\n"
        "Cellular: 3D noise - aka Voronoi\n"
        "Cubic: 3D noise - more random than Perlin?\n"
    );
    Float_knob(f, &_noiseWarp, "noise_warp_strength");
    Tooltip(f,
        "Mult the position data by a simplex noise\n"
        "before generating the main noise, creating\n"
        "swirly patterns."
    );
    Float_knob(f, &_noiseEvolution, "noise_evolution");
    Tooltip(f,
        "For Simplex noise (the only 4D noise), this smoothly\n"
        "changes the noise in a non-spatial dimension (sort \n"
        "of like time...)."
    );
    // fractal functions
    BeginClosedGroup(f, "Fractal");
    Enumeration_knob(f, &_fractalType, fractalTypeNames, "fractal_type");
    Float_knob(f, &_frequency, "frequency");
    Tooltip(f,
    "Affects how coarse the noise output is.\n"
    "Used in all noise generation except White Noise.\n"
    );
    Int_knob(f, &_octaves, "fractal_octaves");
    Tooltip(f,
        "The amount of noise layers used to create the fractal.\n"
        "Used in all fractal noise generation."
    );
    Float_knob(f, &_lacunarity, "lacunarity");
    Tooltip(f,
        "The frequency multiplier between each octave."
    );
    Float_knob(f, &_gain, "fractal_gain");
    Tooltip(f,
        "The relative strength of noise from each layer\n"
        "when compared to the last one."
    );
    EndGroup(f); // Fractal
    // cellular functions
    BeginClosedGroup(f, "Cellular");
    Enumeration_knob(f, &_distanceFunction, distanceFunctionNames, "distance_function");
    Tooltip(f,
        "The distance function used to calculate the cell\n"
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
    Float_knob(f, &_falloff, "falloff");
    Float_knob(f, &_falloffGamma, "falloff_gamma");
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

    Float_knob(f, &_mix, "mix");
}

const char* DeepCPNoise::node_help() const
{
    return
    "Mask a grade op by a world-position pass.";
}

static Op* build(Node* node) { return new DeepCPNoise(node); }
const Op::Description DeepCPNoise::d("DeepCPNoise", 0, build);
