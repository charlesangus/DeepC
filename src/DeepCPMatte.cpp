#include "DeepCMWrapper.h"
#include "DDImage/ViewerContext.h"
#include "DDImage/gl.h"

using namespace DD::Image;

static const char *const sampleId[] = {"closest", "furthest", 0};
static const char *const shapeNames[] = {"sphere", "cube", 0};
static const char *const falloffTypeNames[] = {"smooth", "linear", 0};

enum
{
    CLOSEST,
    FURTHEST
};

enum
{
    SMOOTH,
    LINEAR
};

class DeepCPMatte : public DeepCMWrapper
{

    int _sampleId;
    float _positionPick[2];
    Matrix4 _axisKnob;
    Vector2 _center;
    int _shape;
    int _falloffType;
    float _falloff;
    float _falloffGamma;

public:
    DeepCPMatte(Node *node) : DeepCMWrapper(node), _shape(0), _sampleId(0), _falloffType(0), _falloff(1.0f), _falloffGamma(1.0f)
    {
        _auxChannelKnobName = "position_data";
        _positionPick[0] = _positionPick[1] = 0.0f;
        _center.x = _center.y = 0.0;
        _axisKnob.makeIdentity();
    }

    virtual void wrappedPerSample(
        Box::iterator it,
        size_t sampleNo,
        float alpha,
        DeepPixel deepInPixel,
        float &perSampleData,
        Vector3 &sampleColor);

    virtual void custom_knobs(Knob_Callback f);
    int knob_changed(Knob *k);
    void build_handles(ViewerContext *ctx);
    void draw_handle(ViewerContext *ctx);

    static const Iop::Description d;
    const char *Class() const { return d.name; }
    virtual Op *op() { return this; }
    const char *node_help() const;
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
    float &perSampleData,
    Vector3 &sampleColor)
{
    // generate mask
    float m;
    Vector4 position;
    float distance;

    ChannelSet available;
    available = deepInPixel.channels();

    foreach (z, _auxiliaryChannelSet)
    {
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
            }
            else
            {
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
            position[0] * position[0] + position[1] * position[1] + position[2] * position[2], .5);
    }
    else
    {
        // cube
        distance = 1.0 - (clamp(1 - fabs(position[0])) * clamp(1 - fabs(position[1])) * clamp(1 - fabs(position[2])));
    }
    distance = clamp((1 - distance) / _falloff, 0.0f, 1.0f);
    distance = pow(distance, 1.0f / _falloffGamma);

    // falloff
    if (_falloffType == SMOOTH)
    {
        m = smoothstep(0.0f, 1.0f, distance);
    }
    else if (_falloffType == LINEAR)
    {
        m = clamp(distance, 0.0f, 1.0f);
    }
    perSampleData = m;
}

void DeepCPMatte::build_handles(ViewerContext *ctx)
{
    build_knob_handles(ctx);
    if (ctx->transform_mode() != VIEWER_2D)
        return;
    add_draw_handle(ctx);
}

void DeepCPMatte::draw_handle(ViewerContext *ctx)
{
    if (!ctx->draw_lines())
        return;

    DeepCPMatte::_validate(false);
    glBegin(GL_LINES);
    glVertex2d(_center.x, _center.y);
    glEnd();
}

void DeepCPMatte::custom_knobs(Knob_Callback f)
{
    XY_knob(f, &_center[0], "center", "center");
    Enumeration_knob(f, &_sampleId, sampleId, "sample_area", "sample area");
    BeginGroup(f, "Position");
    Axis_knob(f, &_axisKnob, "selection");
    EndGroup(f);

    Divider(f, "");
    Enumeration_knob(f, &_shape, shapeNames, "shape");
    Enumeration_knob(f, &_falloffType, falloffTypeNames, "falloffType");
    Float_knob(f, &_falloff, "falloff");
    Float_knob(f, &_falloffGamma, "falloff_gamma");
}

int DeepCPMatte::knob_changed(Knob *k)
{
    if (k->is("center"))
    {
        input0()->validate(true);
        Box box(_center.x, _center.y, _center.x + 1, _center.y + 1);

        input0()->deepRequest(box, _auxiliaryChannelSet);
        DeepPlane plane;

        if (!input0()->deepEngine(box, _auxiliaryChannelSet, plane))
        {
            return 0;
        }

        DeepPixel inPixel = plane.getPixel(_center.y, _center.x);
        int inPixelSamples = inPixel.getSampleCount();
        int sampleIdx = _sampleId == CLOSEST ? inPixelSamples - 1 : 0;
        if (inPixelSamples > 0)
        {
            foreach (z, _auxiliaryChannelSet)
            {
                knob("translate")->set_value(inPixel.getOrderedSample(sampleIdx, z), colourIndex(z));
            }
        }
        return 1;
    }
    return DeepFilterOp::knob_changed(k);
}

const char *DeepCPMatte::node_help() const
{
    return "PMatte node for DeepC.";
}

static Op *build(Node *node) { return new DeepCPMatte(node); }
const Op::Description DeepCPMatte::d("DeepCPMatte", 0, build);
