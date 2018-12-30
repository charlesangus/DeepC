#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"

using namespace DD::Image;

class DeepCShuffle : public DeepFilterOp
{
    Channel _inChannel0;
    Channel _inChannel1;
    Channel _inChannel2;
    Channel _inChannel3;

    Channel _outChannel0;
    Channel _outChannel1;
    Channel _outChannel2;
    Channel _outChannel3;

    ChannelSet _inChannelSet;
    ChannelSet _outChannelSet;
public:

    DeepCShuffle(Node* node) : DeepFilterOp(node)
    {
        _inChannel0 = Chan_Red;
        _inChannel1 = Chan_Green;
        _inChannel2 = Chan_Blue;
        _inChannel3 = Chan_Alpha;

        _outChannel0 = Chan_Red;
        _outChannel1 = Chan_Green;
        _outChannel2 = Chan_Blue;
        _outChannel3 = Chan_Alpha;

        _inChannelSet = Chan_Black;
        _outChannelSet = Chan_Black;
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

void DeepCShuffle::_validate(bool for_real)
{
    DeepFilterOp::_validate(for_real);

    _inChannelSet = Chan_Black;
    _inChannelSet += _inChannel0;
    _inChannelSet += _inChannel1;
    _inChannelSet += _inChannel2;
    _inChannelSet += _inChannel3;

    _outChannelSet = Chan_Black;
    _outChannelSet += _outChannel0;
    _outChannelSet += _outChannel1;
    _outChannelSet += _outChannel2;
    _outChannelSet += _outChannel3;

    ChannelSet newChannelSet;
    newChannelSet = _deepInfo.channels();
    newChannelSet += _outChannelSet;
    _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), newChannelSet);
}

void DeepCShuffle::getDeepRequests(Box bbox, const DD::Image::ChannelSet& requestedChannels, int count, std::vector<RequestData>& requests)
{
    ChannelSet modifiedRequestedChannels;
    modifiedRequestedChannels = requestedChannels;
    modifiedRequestedChannels += _inChannelSet;
    requests.push_back(RequestData(input0(), bbox, modifiedRequestedChannels, count));
}

bool DeepCShuffle::doDeepEngine(DD::Image::Box bbox, const DD::Image::ChannelSet& requestedChannels, DeepOutputPlane& deepOutPlane)
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

        Channel inChannel;
        // copy samples to DeepOutputPlane
        for (size_t sampleNo = 0; sampleNo < inPixelSamples; sampleNo++)
        {
            foreach (z, requestedChannels)
            {
                inChannel = z;
                if (z == _outChannel0)
                    inChannel = _inChannel0;
                if (z == _outChannel1)
                    inChannel = _inChannel1;
                if (z == _outChannel2)
                    inChannel = _inChannel2;
                if (z == _outChannel3)
                    inChannel = _inChannel3;
                const float& inData = inPixelChannels.contains(inChannel) && inChannel != Chan_Black
                                      ? deepInPixel.getUnorderedSample(sampleNo, inChannel)
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

void DeepCShuffle::knobs(Knob_Callback f)
{
    Input_Channel_knob(f, &_inChannel0, 1, 0, "in");
    Text_knob(f, ">>"); ClearFlags(f, Knob::STARTLINE);
    Channel_knob(f, &_outChannel0, 1, "out"); ClearFlags(f, Knob::STARTLINE);
    Input_Channel_knob(f, &_inChannel1, 1, 0, "in");
    Text_knob(f, ">>"); ClearFlags(f, Knob::STARTLINE);
    Channel_knob(f, &_outChannel1, 1, "out"); ClearFlags(f, Knob::STARTLINE);
    Input_Channel_knob(f, &_inChannel2, 1, 0, "in");
    Text_knob(f, ">>"); ClearFlags(f, Knob::STARTLINE);
    Channel_knob(f, &_outChannel2, 1, "out"); ClearFlags(f, Knob::STARTLINE);
    Input_Channel_knob(f, &_inChannel3, 1, 0, "in");
    Text_knob(f, ">>"); ClearFlags(f, Knob::STARTLINE);
    Channel_knob(f, &_outChannel3, 1, "out"); ClearFlags(f, Knob::STARTLINE);
}

const char* DeepCShuffle::node_help() const
{
    return
    "Shuffle in Deep. Turning off an output channel passes it through. "
    "Turning off an input channel sets the output channel to black.";
}


static Op* build(Node* node) { return new DeepCShuffle(node); }
const Op::Description DeepCShuffle::d("DeepCShuffle", 0, build);
