#include "DeepCMWrapper.h"
#include "FastNoise.h"

using namespace DD::Image;


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

class DeepCPNoise : public DeepCMWrapper
{

    // knob variables
    Matrix4 _axisKnob;

    // noise related
    int _noiseType;
    float _noiseEvolution;

    // fractals

    int _fractalType; // enum
    float _frequency;
    int _octaves;
    float _lacunarity;
    float _gain;

    int _distanceFunction; // enum
    int _cellularReturnType; // enum
    int _cellularDistanceIndex0;
    int _cellularDistanceIndex1;

    // grade
    float blackpoint[3];
    float whitepoint[3];
    float black[3];
    float white[3];
    float multiply[3];
    float add[3];
    float gamma[3];

    // precalculated grade coefficients
    // set in _validate
    float A[3];
    float B[3];
    float G[3];

    // grade
    bool _reverse;
    bool _blackClamp;
    bool _whiteClamp;

    // noise instance
    // set in _validate
    FastNoise _fastNoise;

    public:

        DeepCPNoise(Node* node) : DeepCMWrapper(node)
        {
            _auxChannelKnobName = "position_data";

            _axisKnob.makeIdentity();

            // noise
            _noiseType = 0;
            _noiseEvolution = 0.0f;

            // fractal
            _fractalType = 0;
            _frequency = .5f;
            _octaves = 5;
            _lacunarity = 2.0f;
            _gain = .5f;

            // cellular
            _distanceFunction = 0;
            _cellularReturnType = 0;
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
            _reverse = false;
            _blackClamp = false;
            _whiteClamp = false;
        }

        virtual void _validate(bool for_real);
        virtual void wrappedPerSample(
            Box::iterator it,
            size_t sampleNo,
            float alpha,
            DeepPixel deepInPixel,
            float &perSampleData
            );
        virtual void wrappedPerChannel(
            const float inputVal,
            float perSampleData,
            Channel z,
            float& outData
            );
        virtual void custom_knobs(Knob_Callback f);

        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};

void DeepCPNoise::_validate(bool for_real)
{
    // must call parent function first
    // in case we want to modify the _deepInfo
    DeepCMWrapper::_validate(for_real);

    // set up the grading stuff
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

    // set up the FastNoise instance

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

}

/*
Do per-sample, channel-agnostic processing. Used for things like generating P
mattes and so on.
TODO: probably better to work with a pointer and length, and then this can
return arrays of data if desired.
*/
void DeepCPNoise::wrappedPerSample(
    Box::iterator it,
    size_t sampleNo,
    float alpha,
    DeepPixel deepInPixel,
    float &perSampleData
    )
{

    // generate noise
    Vector4 position;
    float noiseValue;
    float positionDisturbance[3];
    ChannelSet available;
    available = deepInPixel.channels();
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
    position = position.divide_w(); // TODO: is this necessary? shouldn't be.

    // TODO: handle this better; fragile now - depends on order of noise list
    if (_noiseType==0)
    {
        // GetNoise(x, y, z, w) only implemented for simplex so far
        perSampleData = _fastNoise.GetNoise(position[0], position[1], position[2], _noiseEvolution);
    } else
    {
        perSampleData = _fastNoise.GetNoise(position[0], position[1], position[2]);
    }
    // TODO: used to be then processing the result like this:
    // m = 1.0f - clamp(noiseValue * .5f + .5f, 0.0f, 1.0f);
    // figure out why... just to make a more useful default
    // result?
    perSampleData = perSampleData * .5f + .5f;
}


void DeepCPNoise::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData
    )
{
    int cIndex;
    float gradedPerSampleData;

    cIndex = colourIndex(z);

    if (_reverse)
    {
        // opposite gamma, precomputed
        if (G[cIndex] != 1.0f)
            perSampleData = pow(perSampleData, G[cIndex]);
        // then inverse linear ramp we have already precomputed
        gradedPerSampleData = A[cIndex] * perSampleData + B[cIndex];
    } else
    {
        gradedPerSampleData = A[cIndex] * perSampleData + B[cIndex];
        if (G[cIndex] != 1.0f)
            gradedPerSampleData = pow(gradedPerSampleData, G[cIndex]);
    }

    if (_blackClamp)
        gradedPerSampleData = MAX(gradedPerSampleData, 0.0f);

    if (_whiteClamp)
        gradedPerSampleData = MIN(gradedPerSampleData, 1.0f);


    switch (_operation)
    {
        case REPLACE:
            outData = gradedPerSampleData;
            break;
        case UNION:
            outData = inputVal + gradedPerSampleData - inputVal * gradedPerSampleData;
            break;
        case MASK:
            outData = inputVal * gradedPerSampleData;
            break;
        case STENCIL:
            outData = inputVal * (1.0f - gradedPerSampleData);
            break;
        case OUT:
            outData = (1.0f - inputVal) * gradedPerSampleData;
            break;
        case MIN_OP:
            outData = MIN(inputVal, gradedPerSampleData);
            break;
        case MAX_OP:
            outData = MAX(inputVal, gradedPerSampleData);
            break;
    }
}


void DeepCPNoise::custom_knobs(Knob_Callback f)
{
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
}


const char* DeepCPNoise::node_help() const
{
    return
    "Generate noise to use for grading operations.";
}


static Op* build(Node* node) { return new DeepCPNoise(node); }
const Op::Description DeepCPNoise::d("DeepCPNoise", 0, build);
