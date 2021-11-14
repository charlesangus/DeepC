#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"

using namespace DD::Image;

class DeepCRemoveChannels : public DeepFilterOp
{
    ChannelSet channels;
    ChannelSet channels2;
    ChannelSet channels3;
    ChannelSet channels4;

    int operation; // 0 = remove, 1 = keep

public:

    DeepCRemoveChannels(Node* node) : DeepFilterOp(node)
    {
        channels = Mask_None;
        channels2 = channels3 = channels4 = Mask_None;
        operation = 0;
    }


    void _validate(bool);
    virtual void getDeepRequests(DD::Image::Box bbox, const DD::Image::ChannelSet& requestedChannels, int count, std::vector<RequestData>& requests);
    bool doDeepEngine(DD::Image::Box bbox, const DD::Image::ChannelSet& requestedChannels, DeepOutputPlane& deepOutPlane);

    virtual void knobs(Knob_Callback);
    static const Iop::Description d;
    const char* Class() const { return d.name; }
    virtual Op* op() { return this; }
    const char* node_help() const;
};

static const char* const enums[] = {
  "remove", "keep", nullptr
};

void DeepCRemoveChannels::_validate(bool for_real)
{
    DeepFilterOp::_validate(for_real);

    ChannelSet new_channelset;

    // keep
    if (operation) {
        new_channelset += channels;
        new_channelset += channels2;
        new_channelset += channels3;
        new_channelset += channels4;
        new_channelset += Mask_Alpha | Mask_Deep | Mask_Z;
    
    // remove
    }else{
        new_channelset = _deepInfo.channels();
        new_channelset -= channels;
        new_channelset -= channels2;
        new_channelset -= channels3;
        new_channelset -= channels4;

    }
    _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), new_channelset);
}


void DeepCRemoveChannels::getDeepRequests(Box bbox, const DD::Image::ChannelSet& requestedChannels, int count, std::vector<RequestData>& requests)
{
    //deepInput = dynamic_cast<DeepOp*>(input0());
    requests.push_back(RequestData(input0(), bbox, requestedChannels, count));
}


bool DeepCRemoveChannels::doDeepEngine(DD::Image::Box bbox, const DD::Image::ChannelSet& requestedChannels, DeepOutputPlane& deepOutPlane)
{
    if (!input0())
        return true;

    DeepPlane deepInPlane;
    if (!input0()->deepEngine(bbox, requestedChannels, deepInPlane))
        return false;

    DeepInPlaceOutputPlane inPlaceOutPlane(requestedChannels, bbox);
    inPlaceOutPlane.reserveSamples(deepInPlane.getTotalSampleCount());

    const DD::Image::ChannelSet inputChannels = input0()->deepInfo().channels();

    for (Box::iterator it = bbox.begin(); it != bbox.end(); ++it)
    {
        if (Op::aborted())
            return false; // bail fast on user-interrupt

        // Get the deep pixel from the input plane:
        DeepPixel deepInPixel = deepInPlane.getPixel(it);
        size_t inPixelSamples = deepInPixel.getSampleCount();
        ChannelSet inPixelChannels = deepInPixel.channels();

        inPlaceOutPlane.setSampleCount(it, deepInPixel.getSampleCount());
        DeepOutputPixel outPixel = inPlaceOutPlane.getPixel(it);

        // copy samples to DeepOutputPlane
        for (size_t sampleNo = 0; sampleNo < inPixelSamples; sampleNo++)
        {
            foreach (z, requestedChannels)
            {
                const float& inData = inPixelChannels.contains(z)
                                      ? deepInPixel.getUnorderedSample(sampleNo, z)
                                      : 0.0f;
                float& outData = outPixel.getWritableUnorderedSample(sampleNo, z);
                outData = inData;
            }
        }
    }

    // inPlaceOutPlane.reviseSamples();
    mFnAssert(inPlaceOutPlane.isComplete());
    deepOutPlane = inPlaceOutPlane;
    return true;
}


void DeepCRemoveChannels::knobs(Knob_Callback f)
{
    Enumeration_knob(f, &operation, enums, "operation");
    Tooltip(f, "Remove: the named channels are deleted\n"
                "Keep: all but the named channels are deleted");
    Obsolete_knob(f, "action", "knob operation $value");
    ChannelMask_knob(f, &channels, "channels");
    ChannelMask_knob(f, &channels2, "channels2", "and");
    ChannelMask_knob(f, &channels3, "channels3", "and");
    ChannelMask_knob(f, &channels4, "channels4", "and");
}


const char* DeepCRemoveChannels::node_help() const
{
    return
    "Remove channels from a Deep data stream.";
}


static Op* build(Node* node) { return new DeepCRemoveChannels(node); }
const Op::Description DeepCRemoveChannels::d("DeepCRemoveChannels", 0, build);
