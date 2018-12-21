#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"

using namespace DD::Image;

class DeepCAddChannels : public DeepFilterOp
{
    ChannelSet channels;
    ChannelSet channels2;
    ChannelSet channels3;
    ChannelSet channels4;
public:

    DeepCAddChannels(Node* node) : DeepFilterOp(node)
    {
        channels = Mask_None;
        channels2 = channels3 = channels4 = Mask_None;
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

void DeepCAddChannels::_validate(bool for_real)
{
    DeepFilterOp::_validate(for_real);

    ChannelSet new_channelset;
    new_channelset = _deepInfo.channels();
    new_channelset += channels;
    new_channelset += channels2;
    new_channelset += channels3;
    new_channelset += channels4;
    _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), new_channelset);
}

void DeepCAddChannels::getDeepRequests(Box bbox, const DD::Image::ChannelSet& requestedChannels, int count, std::vector<RequestData>& requests)
{
    //deepInput = dynamic_cast<DeepOp*>(input0());
    requests.push_back(RequestData(input0(), bbox, requestedChannels, count));
}

bool DeepCAddChannels::doDeepEngine(DD::Image::Box bbox, const DD::Image::ChannelSet& requestedChannels, DeepOutputPlane& deepOutPlane)
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

        inPlaceOutPlane.setSampleCount(it, deepInPixel.getSampleCount());
        DeepOutputPixel outPixel = inPlaceOutPlane.getPixel(it);

        // copy samples to DeepOutputPlane
        for (size_t sample = 0; sample < inPixelSamples; sample++)
        {
            const float* inData = deepInPixel.getUnorderedSample(sample);
            float* outData = outPixel.getWritableUnorderedSample(sample);
            foreach (z, requestedChannels)
            {
                if (inputChannels.contains(z))
                {
                    *outData = *inData;
                    ++outData;
                    ++inData;
                } else {
                    *outData = 0.0f;
                    ++outData;
                }
            }
        }
    }

    // inPlaceOutPlane.reviseSamples();
    mFnAssert(inPlaceOutPlane.isComplete());
    deepOutPlane = inPlaceOutPlane;
    return true;
}

void DeepCAddChannels::knobs(Knob_Callback f)
{
    ChannelMask_knob(f, &channels, "channels");
    ChannelMask_knob(f, &channels2, "channels2", "and");
    ChannelMask_knob(f, &channels3, "channels3", "and");
    ChannelMask_knob(f, &channels4, "channels4", "and");
}

const char* DeepCAddChannels::node_help() const
{
    return
    "Add Channels to a Deep data stream.";
}


static Op* build(Node* node) { return new DeepCAddChannels(node); }
const Op::Description DeepCAddChannels::d("DeepCAddChannels", 0, build);
