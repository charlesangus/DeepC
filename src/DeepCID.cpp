#include "DeepCMWrapper.h"

using namespace DD::Image;

static const char *const sampleId[] = {"closest", "furthest", 0};
static const char* const shapeNames[] = { "sphere", "cube", 0 };
static const char* const falloffTypeNames[] = { "smooth", "linear", 0 };
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

class DeepCID : public DeepCMWrapper
{
    // const char* _auxChannelKnobName;
    Channel _auxChannel;

    float _deepID;
    float _tolerance;
    int _sampleId;
    Vector2 _pick;

public:
    DeepCID(Node *node) : DeepCMWrapper(node), _auxChannel(Chan_Black), _deepID(0.0f), _tolerance(0.5f)
    {
        _auxChannelKnobName = "id_channel";
        _pick.x = _pick.y = 0.0;
        _sampleId = 0;
        _deepID = 0.0f;
    }

        virtual void wrappedPerSample(
            Box::iterator it,
            size_t sampleNo,
            float alpha,
            DeepPixel deepInPixel,
            float &perSampleData,
            Vector3& sampleColor
            );

        virtual void _validate(bool for_real);
        virtual void top_knobs(Knob_Callback f);
        virtual void custom_knobs(Knob_Callback f);
        int knob_changed(Knob *k);

        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};

void DeepCID::_validate(bool for_real)
{
    _auxiliaryChannelSet = Chan_Black;
    _auxiliaryChannelSet += _auxChannel;
    DeepCMWrapper::_validate(for_real);
}

void DeepCID::wrappedPerSample(
    Box::iterator it,
    size_t sampleNo,
    float alpha,
    DeepPixel deepInPixel,
    float &perSampleData,
    Vector3& sampleColor
    )
{
    // generate mask
    float idData;

    ChannelSet available;
    available = deepInPixel.channels();

    perSampleData = 0.0f;
    foreach(z, _auxiliaryChannelSet)
    {
        if (available.contains(_auxChannel))
        {
            if (_unpremultPosition)
            {
                idData = deepInPixel.getUnorderedSample(sampleNo, _auxChannel) / alpha;
            } else {
                idData = deepInPixel.getUnorderedSample(sampleNo, _auxChannel);
            }

            if (fabs(idData - _deepID) < _tolerance)
            {
                perSampleData = 1.0f;
            }
        }
    }
}

void DeepCID::top_knobs(Knob_Callback f)
{
    // _auxiliaryChannelSet is actually a channel in this case, but they mostly
    // work similarly, afaict
    Input_Channel_knob(f, &_auxChannel, 1, 0, _auxChannelKnobName);
    Bool_knob(f, &_unpremultPosition, "unpremult_id", "unpremult id");
    Tooltip(f, "Uncheck for ScanlineRender Deep data, check for (probably) "
    "all other renderers. Nuke stores position data from the ScanlineRender "
    "node unpremultiplied, contrary to the Deep spec. Other renderers "
    "presumably store position data (and all other data) premultiplied, "
    "as required by the Deep spec.");
    Input_ChannelSet_knob(f, &_processChannelSet, 0, "output");
    Bool_knob(f, &_unpremult, "unpremult", "(un)premult");
    Tooltip(f, "If, for some reason, you want your mask stored without "
    "premultipling it, contrary to the Deep spec, uncheck this. "
    "Should probably always be checked.");
    Enumeration_knob(f, &_operation, operationNames, "operation");
    Tooltip(f, "Operation to provide picked mask.");
    Divider(f, "");
}

void DeepCID::custom_knobs(Knob_Callback f)
{
    XY_knob(f, &_pick[0], "id_pick", "id picker");
    Tooltip(f, "Knob to provied pick possibility.");
    SetFlags(f, Knob::DISABLED);
    Enumeration_knob(f, &_sampleId, sampleId, "sample_area", "sample area");
    Tooltip(f, "Select if closest or furthest sample is used for picking.");
    Float_knob(f, &_deepID, "id");
    Tooltip(f, "Actual Deep ID to use for mask creation.");
    Float_knob(f, &_tolerance, "tolerance");
    Tooltip(f, "Tolerance to use for mask separation between values.");
}

int DeepCID::knob_changed(Knob *k)
{
    if (k->is("id_pick"))
    {
        input0()->validate(true);
        Box box(_pick.x, _pick.y, _pick.x + 1, _pick.y + 1);

        input0()->deepRequest(box, _auxiliaryChannelSet);
        DeepPlane plane;

        if (!input0()->deepEngine(box, _auxiliaryChannelSet, plane))
        {
            return 0;
        }

        DeepPixel inPixel = plane.getPixel(_pick.y, _pick.x);
        int inPixelSamples = inPixel.getSampleCount();
        int sampleIdx = _sampleId == CLOSEST ? inPixelSamples - 1 : 0;
        if (inPixelSamples > 0)
        {
            knob("id")->set_value(inPixel.getOrderedSample(sampleIdx, _auxiliaryChannelSet.first()));
        }
        return 1;
    }
    return DeepFilterOp::knob_changed(k);
}
const char *DeepCID::node_help() const
{
    return "Convert arbitrary IDs to mattes.";
}

static Op *build(Node *node) { return new DeepCID(node); }
const Op::Description DeepCID::d("DeepCID", 0, build);
