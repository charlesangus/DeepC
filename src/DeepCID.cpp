#include "DeepCMWrapper.h"

using namespace DD::Image;

static const char* const shapeNames[] = { "sphere", "cube", 0 };
static const char* const falloffTypeNames[] = { "smooth", "linear", 0 };
enum { SMOOTH, LINEAR };

class DeepCID : public DeepCMWrapper
{
    // const char* _auxChannelKnobName;
    Channel _auxChannel;

    float _deepID;
    float _tolerance;

    public:

        DeepCID(Node* node) : DeepCMWrapper(node)
            , _auxChannel(Chan_Black)
            , _deepID(0.0f)
            , _tolerance(0.5f)
        {
            _auxChannelKnobName = "id_channel";
        }

        virtual void wrappedPerSample(
            Box::iterator it,
            size_t sampleNo,
            float alpha,
            DeepPixel deepInPixel,
            float &perSampleData
            );

        virtual void _validate(bool for_real);
        virtual void top_knobs(Knob_Callback f);
        virtual void custom_knobs(Knob_Callback f);

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
    float &perSampleData
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
    Divider(f, "");
}

void DeepCID::custom_knobs(Knob_Callback f)
{
    Float_knob(f, &_deepID, "id");
    Float_knob(f, &_tolerance, "tolerance");
}


const char* DeepCID::node_help() const
{
    return
    "Convert arbitrary IDs to mattes.";
}


static Op* build(Node* node) { return new DeepCID(node); }
const Op::Description DeepCID::d("DeepCID", 0, build);
