#include "DeepCMWrapper.h"

using namespace DD::Image;

static const char* const shapeNames[] = { "sphere", "cube", 0 };
static const char* const falloffTypeNames[] = { "smooth", "linear", 0 };
enum { SMOOTH, LINEAR };

class DeepCPMatte : public DeepCMWrapper
{

    float _positionPick[2];
    Matrix4 _axisKnob;

    int _shape;
    int _falloffType;
    float _falloff;
    float _falloffGamma;

    public:

        DeepCPMatte(Node* node) : DeepCMWrapper(node)
            , _shape(0)
            , _falloffType(0)
            , _falloff(1.0f)
            , _falloffGamma(1.0f)
        {
            _auxChannelKnobName = "position_data";
            _positionPick[0] = _positionPick[1] = 0.0f;
            _axisKnob.makeIdentity();
        }

        virtual void wrappedPerSample(
            Box::iterator it,
            size_t sampleNo,
            float alpha,
            DeepPixel deepInPixel,
            float &perSampleData
            );

        virtual void custom_knobs(Knob_Callback f);

        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};

/*
Do per-sample, channel-agnostic processing. Used for things like generating P
mattes and so on.
TODO: probably better to work with a pointer and length, and then this can
return arrays of data if desired.
*/
void DeepCPMatte::wrappedPerSample(
    Box::iterator it,
    size_t sampleNo,
    float alpha,
    DeepPixel deepInPixel,
    float &perSampleData
    )
{
    // generate mask
    float m;
    Vector4 position;
    float distance;

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
    position = position.divide_w();

    if (_shape == 0)
    {
        // sphere
        distance = pow(
            position[0] * position[0]
            + position[1] * position[1]
            + position[2] * position[2]
            , .5);
    } else
    {
        // cube
        distance =
            clamp(1-fabs(position[0]))
            * clamp(1-fabs(position[1]))
            * clamp(1-fabs(position[2]));
    }
    distance = clamp((1 - distance) / _falloff, 0.0f, 1.0f);
    distance = pow(distance, 1.0f / _falloffGamma);

    // falloff
    if (_falloffType == SMOOTH)
    {
        m = smoothstep(0.0f, 1.0f, distance);
    } else if (_falloffType == LINEAR)
    {
        m = clamp(distance, 0.0f, 1.0f);
    }
    perSampleData = m;
}

void DeepCPMatte::custom_knobs(Knob_Callback f)
{
    BeginGroup(f, "Position");
    Axis_knob(f, &_axisKnob, "selection");
    EndGroup(f); // Position

    Divider(f, "");
    Enumeration_knob(f, &_shape, shapeNames, "shape");
    Enumeration_knob(f, &_falloffType, falloffTypeNames, "falloffType");
    Float_knob(f, &_falloff, "falloff");
    Float_knob(f, &_falloffGamma, "falloff_gamma");
}


const char* DeepCPMatte::node_help() const
{
    return
    "PMatte node for DeepC.";
}


static Op* build(Node* node) { return new DeepCPMatte(node); }
const Op::Description DeepCPMatte::d("DeepCPMatte", 0, build);
