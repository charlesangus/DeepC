#include "DeepCMWrapper.h"

using namespace DD::Image;

void DeepCMWrapper::findNeededDeepChannels(ChannelSet& neededDeepChannels)
{
    DeepCWrapper::findNeededDeepChannels(neededDeepChannels);
    neededDeepChannels += _auxiliaryChannelSet;
}

void DeepCMWrapper::_validate(bool for_real)
{
    DeepCWrapper::_validate(for_real);

    // add our output channels to the _deepInfo
    ChannelSet new_channelset;
    new_channelset = _deepInfo.channels();
    new_channelset += _processChannelSet;
    _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), new_channelset);
}

/*
Do per-sample, channel-agnostic processing. Used for things like generating P
mattes and so on.
TODO: probably better to work with a pointer and length, and then this can
return arrays of data if desired.
*/
void DeepCMWrapper::wrappedPerSample(
    Box::iterator it,
    size_t sampleNo,
    float alpha,
    DeepPixel deepInPixel,
    float &perSampleData,
    Vector3& sampleColor
    )
{
    perSampleData = 1.0f;
}


/*
The guts. Do any processing on the channel value. The result will be masked
and mixed appropriately.
*/
void DeepCMWrapper::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData,
    Vector3& sampleColor
    )
{
    switch (_operation)
    {
        case REPLACE:
            outData = perSampleData;
            break;
        case UNION:
            outData = inputVal + perSampleData - inputVal * perSampleData;
            break;
        case MASK:
            outData = inputVal * perSampleData;
            break;
        case STENCIL:
            outData = inputVal * (1.0f - perSampleData);
            break;
        case OUT:
            outData = (1.0f - inputVal) * perSampleData;
            break;
        case MIN_OP:
            outData = MIN(inputVal, perSampleData);
            break;
        case MAX_OP:
            outData = MAX(inputVal, perSampleData);
            break;
    }
}


void DeepCMWrapper::top_knobs(Knob_Callback f)
{
    Input_ChannelSet_knob(f, &_auxiliaryChannelSet, 0, _auxChannelKnobName);
    Bool_knob(f, &_unpremultPosition, "unpremult_position_data", "unpremult position");
    Tooltip(f, "Uncheck for ScanlineRender Deep data, check for (probably) "
    "all other renderers. Nuke stores position data from the ScanlineRender "
    "node unpremultiplied, contrary to the Deep spec. Other renderers "
    "presumably store position data (and all other data) premultiplied, "
    "as required by the Deep spec.");
    Input_ChannelSet_knob(f, &_processChannelSet, 0, "output");
    Bool_knob(f, &_unpremult, "unpremult", "(un)premult by alpha");
    Tooltip(f, "If, for some reason, you want your mask stored without "
    "premultipling it, contrary to the Deep spec, uncheck this. "
    "Should probably always be checked.");
    Enumeration_knob(f, &_operation, operationNames, "operation");
    Divider(f, "");
}


void DeepCMWrapper::custom_knobs(Knob_Callback f)
{
}


const char* DeepCMWrapper::node_help() const
{
    return
    "Parent class for matte-generation nodes.";
}

static Op* build(Node* node) { return new DeepCMWrapper(node); }
const Op::Description DeepCMWrapper::d("DeepCMWrapper", 0, build);
