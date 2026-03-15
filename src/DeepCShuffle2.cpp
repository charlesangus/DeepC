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
    ChannelSet  _in1ChannelSet;    // defaults to Mask_RGBA
    ChannelSet  _in2ChannelSet;    // defaults to Chan_Black (none)
    ChannelSet  _out1ChannelSet;   // defaults to Mask_RGBA
    ChannelSet  _out2ChannelSet;   // defaults to Chan_Black (none)
    std::string _matrixState;      // serialized routing, owned by Op; mirrored by knob store()

    // Populated in _validate() by parsing _matrixState.
    // Up to 8 active routing slots. Each slot specifies an output channel and
    // either a source channel to copy from, or a constant 0.0 / 1.0 value.
    std::array<Channel, 8> _outputChannels;      // active output channels
    std::array<Channel, 8> _sourceChannels;      // source channel per output slot (Chan_Black if constant)
    std::array<bool,    8> _sourceIsConstant;    // true if this slot outputs a constant value
    std::array<float,   8> _sourceConstantValue; // constant value (0.0 or 1.0) when _sourceIsConstant
    int _activeSlots;                            // number of active routing slots

public:

    DeepCShuffle2(Node* node) : DeepFilterOp(node)
    {
        _in1ChannelSet  = Mask_RGBA;
        _in2ChannelSet  = Chan_Black;
        _out1ChannelSet = Mask_RGBA;
        _out2ChannelSet = Chan_Black;
        _activeSlots    = 0;
        _outputChannels.fill(Chan_Black);
        _sourceChannels.fill(Chan_Black);
        _sourceIsConstant.fill(false);
        _sourceConstantValue.fill(0.0f);
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
    _sourceIsConstant.fill(false);
    _sourceConstantValue.fill(0.0f);
    _activeSlots = 0;

    if (_matrixState.empty())
    {
        // No routing — output equals input; nothing to add to DeepInfo.
        return;
    }

    // Parse "outName:srcName,outName:srcName,..." up to 8 slots.
    // srcName may be "const:0" or "const:1" for constant column outputs.
    std::istringstream tokenStream(_matrixState);
    std::string token;
    while (_activeSlots < 8 && std::getline(tokenStream, token, ','))
    {
        if (token.empty())
            continue;

        const std::string::size_type colonPos = token.find(':');
        if (colonPos == std::string::npos)
            continue;

        const std::string outName = token.substr(0, colonPos);
        const std::string srcName = token.substr(colonPos + 1);

        // Resolve the output channel — must be a real channel name.
        Channel outputChannel = findChannel(outName.c_str());
        if (outputChannel == Chan_Black)
            continue;

        if (srcName == "const:0")
        {
            _outputChannels[_activeSlots]      = outputChannel;
            _sourceChannels[_activeSlots]      = Chan_Black;
            _sourceIsConstant[_activeSlots]    = true;
            _sourceConstantValue[_activeSlots] = 0.0f;
            ++_activeSlots;
        }
        else if (srcName == "const:1")
        {
            _outputChannels[_activeSlots]      = outputChannel;
            _sourceChannels[_activeSlots]      = Chan_Black;
            _sourceIsConstant[_activeSlots]    = true;
            _sourceConstantValue[_activeSlots] = 1.0f;
            ++_activeSlots;
        }
        else
        {
            // Regular channel-to-channel routing.
            Channel sourceChannel = findChannel(srcName.c_str());
            if (sourceChannel == Chan_Black)
                continue;

            _outputChannels[_activeSlots]      = outputChannel;
            _sourceChannels[_activeSlots]      = sourceChannel;
            _sourceIsConstant[_activeSlots]    = false;
            _sourceConstantValue[_activeSlots] = 0.0f;
            ++_activeSlots;
        }
    }

    // Build the output channel set: start from existing input channels and add
    // any routed output channels so downstream ops can request them.
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
        // Only request channels from input when the source is not a constant.
        if (!_sourceIsConstant[routeIndex] && _sourceChannels[routeIndex] != Chan_Black)
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
        if (!_sourceIsConstant[routeIndex] && _sourceChannels[routeIndex] != Chan_Black)
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
                float outputValue = 0.0f;

                // Check whether this output channel has a routing override.
                bool routeFound = false;
                for (int routeIndex = 0; routeIndex < _activeSlots; ++routeIndex)
                {
                    if (_outputChannels[routeIndex] != Chan_Black &&
                        z == _outputChannels[routeIndex])
                    {
                        if (_sourceIsConstant[routeIndex])
                        {
                            // Write the constant value (0.0 or 1.0).
                            outputValue = _sourceConstantValue[routeIndex];
                        }
                        else
                        {
                            const Channel sourceChannel = _sourceChannels[routeIndex];
                            outputValue =
                                (inPixelChannels.contains(sourceChannel) && sourceChannel != Chan_Black)
                                ? deepInPixel.getUnorderedSample(sampleNo, sourceChannel)
                                : 0.0f;
                        }
                        routeFound = true;
                        break;
                    }
                }

                if (!routeFound)
                {
                    // No routing override — passthrough: copy the input channel unchanged.
                    outputValue =
                        (inPixelChannels.contains(z) && z != Chan_Black)
                        ? deepInPixel.getUnorderedSample(sampleNo, z)
                        : 0.0f;
                }

                outPixel.getWritableUnorderedSample(sampleNo, z) = outputValue;
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
    Input_ChannelSet_knob(f, &_in1ChannelSet, 0, "in1", "in 1");
    SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);

    Input_ChannelSet_knob(f, &_in2ChannelSet, 0, "in2", "in 2");
    SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);

    Input_ChannelSet_knob(f, &_out1ChannelSet, 0, "out1", "out 1");
    SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);

    Input_ChannelSet_knob(f, &_out2ChannelSet, 0, "out2", "out 2");
    SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);

    CustomKnob2(ShuffleMatrixKnob, f, &_matrixState, "matrix", "routing");
}

// -----------------------------------------------------------------------------
// knob_changed
// -----------------------------------------------------------------------------
int DeepCShuffle2::knob_changed(Knob* k)
{
    if (k->is("in1") || k->is("in2") || k->is("out1") || k->is("out2") ||
        k == &Knob::inputChange || k == &Knob::showPanel)
    {
        // Push all four ChannelSets into the matrix knob so the widget can build
        // accurate column headers and row labels without calling back into the Op.
        if (Knob* matrixKnob = knob("matrix"))
        {
            auto* shuffleKnob = dynamic_cast<ShuffleMatrixKnob*>(matrixKnob);
            if (shuffleKnob)
                shuffleKnob->setChannelSets(_in1ChannelSet, _in2ChannelSet,
                                            _out1ChannelSet, _out2ChannelSet);
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
        "Shuffle channels in Deep. Use the in1/in2 pickers to select source layers "
        "and the out1/out2 pickers to select output layers. "
        "Toggle cells in the matrix to route source channels to output channels. "
        "Columns are source channels (with constant 0 and 1 columns); "
        "rows are output channels. "
        "Unrouted channels pass through unchanged.";
}

// -----------------------------------------------------------------------------
// Plugin registration
// -----------------------------------------------------------------------------
static Op* build(Node* node) { return new DeepCShuffle2(node); }
const Op::Description DeepCShuffle2::d("DeepCShuffle2", 0, build);
