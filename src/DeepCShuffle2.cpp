#include "ShuffleMatrixKnob.h"
#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"
#include <array>
#include <string>
#include <sstream>

using namespace DD::Image;

class DeepCShuffle2 : public DeepFilterOp
{
    ChannelSet  _in1ChannelSet;   // defaults to Mask_RGBA
    ChannelSet  _in2ChannelSet;   // defaults to Chan_Black (none)
    std::string _matrixState;     // serialized routing, owned by Op; mirrored by knob store()

    // Populated in _validate() by parsing _matrixState
    std::array<Channel, 8> _outputChannels;  // active output channels
    std::array<Channel, 8> _sourceChannels;  // source channel per output slot
    int _activeSlots;                        // number of active routing slots

public:

    DeepCShuffle2(Node* node) : DeepFilterOp(node)
    {
        _in1ChannelSet = Mask_RGBA;
        _in2ChannelSet = Chan_Black;
        _activeSlots   = 0;
        _outputChannels.fill(Chan_Black);
        _sourceChannels.fill(Chan_Black);
    }

    void _validate(bool for_real) override;
    void getDeepRequests(DD::Image::Box bbox,
                         const DD::Image::ChannelSet& requestedChannels,
                         int count,
                         std::vector<RequestData>& requests) override;
    bool doDeepEngine(DD::Image::Box bbox,
                      const DD::Image::ChannelSet& requestedChannels,
                      DeepOutputPlane& deepOutPlane) override;

    void knobs(Knob_Callback f) override;
    int  knob_changed(Knob* k) override;

    static const Iop::Description d;
    const char* Class() const override { return d.name; }
    Op* op() override { return this; }
    const char* node_help() const override;
};

// -----------------------------------------------------------------------------
// _validate
// -----------------------------------------------------------------------------
void DeepCShuffle2::_validate(bool for_real)
{
    DeepFilterOp::_validate(for_real);

    // Reset routing arrays
    _outputChannels.fill(Chan_Black);
    _sourceChannels.fill(Chan_Black);
    _activeSlots = 0;

    if (_matrixState.empty())
    {
        // No routing — output equals input; nothing to add to DeepInfo
        return;
    }

    // Parse "outName:srcName,outName:srcName,..." up to 8 slots
    std::istringstream tokenStream(_matrixState);
    std::string token;
    while (_activeSlots < 8 && std::getline(tokenStream, token, ','))
    {
        if (token.empty())
            continue;

        std::string::size_type colonPos = token.find(':');
        if (colonPos == std::string::npos)
            continue;

        std::string outName = token.substr(0, colonPos);
        std::string srcName = token.substr(colonPos + 1);

        Channel outputChannel = getChannel(outName.c_str());
        Channel sourceChannel = getChannel(srcName.c_str());

        if (outputChannel == Chan_Black || sourceChannel == Chan_Black)
            continue;

        _outputChannels[_activeSlots] = outputChannel;
        _sourceChannels[_activeSlots] = sourceChannel;
        ++_activeSlots;
    }

    // Build the output channel set: start from existing input channels and add
    // any routed output channels so downstream ops can request them
    ChannelSet newChannelSet = _deepInfo.channels();
    for (int routeIndex = 0; routeIndex < _activeSlots; ++routeIndex)
    {
        if (_outputChannels[routeIndex] != Chan_Black)
            newChannelSet += _outputChannels[routeIndex];
    }
    _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), newChannelSet);
}

// -----------------------------------------------------------------------------
// getDeepRequests
// -----------------------------------------------------------------------------
void DeepCShuffle2::getDeepRequests(Box bbox,
                                    const DD::Image::ChannelSet& requestedChannels,
                                    int count,
                                    std::vector<RequestData>& requests)
{
    ChannelSet neededChannels = requestedChannels;
    for (int routeIndex = 0; routeIndex < _activeSlots; ++routeIndex)
    {
        if (_sourceChannels[routeIndex] != Chan_Black)
            neededChannels += _sourceChannels[routeIndex];
    }
    requests.push_back(RequestData(input0(), bbox, neededChannels, count));
}

// -----------------------------------------------------------------------------
// doDeepEngine
// -----------------------------------------------------------------------------
bool DeepCShuffle2::doDeepEngine(DD::Image::Box bbox,
                                 const DD::Image::ChannelSet& requestedChannels,
                                 DeepOutputPlane& deepOutPlane)
{
    if (!input0())
        return true;

    ChannelSet neededChannels = requestedChannels;
    for (int routeIndex = 0; routeIndex < _activeSlots; ++routeIndex)
    {
        if (_sourceChannels[routeIndex] != Chan_Black)
            neededChannels += _sourceChannels[routeIndex];
    }

    DeepPlane deepInPlane;
    if (!input0()->deepEngine(bbox, neededChannels, deepInPlane))
        return false;

    DeepInPlaceOutputPlane inPlaceOutPlane(requestedChannels, bbox);
    inPlaceOutPlane.reserveSamples(deepInPlane.getTotalSampleCount());

    for (Box::iterator it = bbox.begin(); it != bbox.end(); ++it)
    {
        if (Op::aborted())
            return false;

        DeepPixel deepInPixel = deepInPlane.getPixel(it);
        size_t inPixelSamples = deepInPixel.getSampleCount();
        ChannelSet inPixelChannels = deepInPixel.channels();

        inPlaceOutPlane.setSampleCount(it, inPixelSamples);
        DeepOutputPixel outPixel = inPlaceOutPlane.getPixel(it);

        for (size_t sampleNo = 0; sampleNo < inPixelSamples; ++sampleNo)
        {
            foreach (z, requestedChannels)
            {
                // Default: passthrough — unrouted channels are never zeroed
                Channel inChannel = z;

                // Check whether this output channel has a routing override
                for (int routeIndex = 0; routeIndex < _activeSlots; ++routeIndex)
                {
                    if (_outputChannels[routeIndex] != Chan_Black &&
                        z == _outputChannels[routeIndex])
                    {
                        inChannel = _sourceChannels[routeIndex];
                        break;
                    }
                }

                const float& inData =
                    inPixelChannels.contains(inChannel) && inChannel != Chan_Black
                    ? deepInPixel.getUnorderedSample(sampleNo, inChannel)
                    : 0.0f;

                outPixel.getWritableUnorderedSample(sampleNo, z) = inData;
            }
        }
    }

    mFnAssert(inPlaceOutPlane.isComplete());
    deepOutPlane = inPlaceOutPlane;
    return true;
}

// -----------------------------------------------------------------------------
// knobs
// -----------------------------------------------------------------------------
void DeepCShuffle2::knobs(Knob_Callback f)
{
    Input_ChannelSet_knob(f, &_in1ChannelSet, 0, "in1", "in1 channels");
    SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);

    Input_ChannelSet_knob(f, &_in2ChannelSet, 0, "in2", "in2 channels");
    SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);

    CustomKnob2(ShuffleMatrixKnob, f, &_matrixState, "matrix", "routing");
}

// -----------------------------------------------------------------------------
// knob_changed
// -----------------------------------------------------------------------------
int DeepCShuffle2::knob_changed(Knob* k)
{
    if (k->is("in1") || k->is("in2") ||
        k == &Knob::inputChange || k == &Knob::showPanel)
    {
        // Push current ChannelSets into the matrix knob so widget column
        // headers update without the widget needing to call back into the Op
        if (Knob* matrixKnob = knob("matrix"))
        {
            auto* shuffleKnob = dynamic_cast<ShuffleMatrixKnob*>(matrixKnob);
            if (shuffleKnob)
                shuffleKnob->setChannelSets(_in1ChannelSet, _in2ChannelSet);
            matrixKnob->updateWidgets();
        }
        return 1;
    }
    return DeepFilterOp::knob_changed(k);
}

// -----------------------------------------------------------------------------
// node_help
// -----------------------------------------------------------------------------
const char* DeepCShuffle2::node_help() const
{
    return
        "Shuffle channels in Deep. Use the in1/in2 pickers to select source layers. "
        "Toggle cells in the matrix to route source channels to output channels. "
        "Unrouted channels pass through unchanged.";
}

// -----------------------------------------------------------------------------
// Plugin registration
// -----------------------------------------------------------------------------
static Op* build(Node* node) { return new DeepCShuffle2(node); }
const Op::Description DeepCShuffle2::d("DeepCShuffle2", 0, build);
