#include "DeepCWrapper.h"

using namespace DD::Image;

/*
Get all the channels we need - in addition to any requested from downstream -
together in one convenient place. Subclasses should call the parent
implementation before their own.
*/
void DeepCWrapper::findNeededDeepChannels(ChannelSet& neededDeepChannels)
{
    neededDeepChannels = Chan_Black;
    neededDeepChannels += _processChannelSet;
    if (_doDeepMask)
        neededDeepChannels += _deepMaskChannel;
    if (_unpremult || _unpremultDeepMask)
        neededDeepChannels += Chan_Alpha;
    neededDeepChannels += Chan_DeepBack;
    neededDeepChannels += Chan_DeepFront;
}

/*
Handles validating the flat mask, setting up channels, making safe input values.
Subclasses should call the parent implementation and then their own, if they
need to do their own validation. This gives subclasses a chance to work with
the DeepInfo object to e.g. add more channels into the stream.
*/
void DeepCWrapper::_validate(bool for_real)
{
    // make safe the mix - values outside of 0-1 are meaningless
    _mix = clamp(_mix, 0.0f, 1.0f);

    _maskOp = dynamic_cast<Iop*>(Op::input(1));
    if (_maskOp != NULL && _sideMaskChannel != Chan_Black)
    {
        _maskOp->validate(for_real);
        _doSideMask = true;
    } else {
        _doSideMask = false;
    }

    if (_deepMaskChannel != Chan_Black)
    {
        _doDeepMask = true;
    }
    else
    {
        _doDeepMask = false;
    }

    // set up our needed channels
    findNeededDeepChannels(_allNeededDeepChannels);

    DeepFilterOp::_validate(for_real);
}


void DeepCWrapper::getDeepRequests(
    Box bbox,
    const DD::Image::ChannelSet& channels,
    int count,
    std::vector<RequestData>& requests
    )
{
    if (!input0())
        return;
    DD::Image::ChannelSet requestChannels = channels;
    requestChannels += _allNeededDeepChannels;
    requests.push_back(RequestData(input0(), bbox, requestChannels, count));

    if (_doSideMask)
        _maskOp->request(bbox, _sideMaskChannel, count);
}

/*
Do per-sample, channel-agnostic processing. Used for things like generating P
mattes and so on.
TODO: probably better to work with a pointer and length, and then this can
return arrays of data if desired.
*/
void DeepCWrapper::wrappedPerSample(
    Box::iterator it,
    size_t sampleNo,
    float alpha,
    DeepPixel deepInPixel,
    float &perSampleData,
    Vector3 &sampleColor
    )
{
    perSampleData = 1.0f;
}


/*
The guts. Do any processing on the channel value. The result will be masked
and mixed appropriately.
*/
void DeepCWrapper::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData,
    Vector3 &sampleColor
    )
{
    outData = inputVal * _gain;
}


bool DeepCWrapper::doDeepEngine(
    Box bbox,
    const DD::Image::ChannelSet& requestedChannels,
    DeepOutputPlane& deepOutPlane
    )
{
    if (!input0())
        return true;

    DD::Image::ChannelSet getChannels = requestedChannels;
    getChannels += _allNeededDeepChannels;

    DeepPlane deepInPlane;
    if (!input0()->deepEngine(bbox, getChannels, deepInPlane))
        return false;

    ChannelSet available;
    available = deepInPlane.channels();

    DeepInPlaceOutputPlane inPlaceOutPlane(requestedChannels, bbox);
    inPlaceOutPlane.reserveSamples(deepInPlane.getTotalSampleCount());
    const DD::Image::ChannelSet inputChannels = input0()->deepInfo().channels();

    const int nOutputChans = requestedChannels.size();

    // mask input stuff
    float sideMaskVal;
    int currentYRow;
    Row maskRow(bbox.x(), bbox.r());

    if (_doSideMask)
    {
        _maskOp->get(bbox.y(), bbox.x(), bbox.r(), _sideMaskChannel, maskRow);
        currentYRow = bbox.y();
    }

    for (Box::iterator it = bbox.begin(); it != bbox.end(); ++it)
    {
        if (Op::aborted())
            return false; // bail fast on user-interrupt

        // Get the deep pixel from the input plane:
        DeepPixel deepInPixel = deepInPlane.getPixel(it);
        size_t inPixelSamples = deepInPixel.getSampleCount();

        inPlaceOutPlane.setSampleCount(it, deepInPixel.getSampleCount());
        DeepOutputPixel outPixel = inPlaceOutPlane.getPixel(it);

        // flat masking
        sideMaskVal = 1.0f;
        if (_doSideMask)
        {
            if (currentYRow != it.y)
            {
                // we have not already gotten this row, get it now
                _maskOp->get(it.y, bbox.x(), bbox.r(), _sideMaskChannel, maskRow);
                sideMaskVal = maskRow[_sideMaskChannel][it.x];
                sideMaskVal = clamp(sideMaskVal);
                currentYRow = it.y;
            } else
            {
                // we've already got this row, just get the value
                sideMaskVal = maskRow[_sideMaskChannel][it.x];
                sideMaskVal = clamp(sideMaskVal);
            }
            if (_invertSideMask)
                sideMaskVal = 1.0f - sideMaskVal;
        }

        // for each sample
        for (size_t sampleNo = 0; sampleNo < inPixelSamples; sampleNo++)
        {

            // alpha
            float alpha;
            alpha = 1.0f;
            if (
                available.contains(Chan_Alpha)
                && (_unpremult || _unpremultDeepMask)
                )
            {
                alpha = deepInPixel.getUnorderedSample(sampleNo, Chan_Alpha);
            }

            // deep masking
            float deepMaskVal;
            deepMaskVal = 1.0f;
            if (_doDeepMask)
            {
                if (available.contains(_deepMaskChannel))
                {
                    deepMaskVal = deepInPixel.getUnorderedSample(sampleNo, _deepMaskChannel);
                    if (_unpremultDeepMask)
                    {
                        if (alpha != 0.0f)
                        {
                            deepMaskVal /= alpha;
                        } else
                        {
                            deepMaskVal = 0.0f;
                        }
                    }
                    if (_invertDeepMask)
                        deepMaskVal = 1.0f - deepMaskVal;
                } else
                {
                    deepMaskVal = 0.0f;
                }
            }

            float perSampleData;
            Vector3 sampleColor;
            wrappedPerSample(it, sampleNo, alpha, deepInPixel, perSampleData, sampleColor);

            // process the sample
            float inputVal;
            int cIndex;
            // for each channel
            ChannelSet inPixelChannels;
            inPixelChannels = deepInPixel.channels();
            foreach(z, requestedChannels)
            {
                cIndex = colourIndex(z);


                const float& inData = inPixelChannels.contains(z)
                                      ? deepInPixel.getUnorderedSample(sampleNo, z)
                                      : 0.0f;
                float& outData = outPixel.getWritableUnorderedSample(sampleNo, z);

                // channels we know we should pass through
                if (
                       z == Chan_Alpha
                    || z == Chan_Z
                    || z == Chan_DeepFront
                    || z == Chan_DeepBack
                    || _mix == 0.0f
                    || (_doDeepMask && deepMaskVal == 0.0f)
                    || (_doSideMask && sideMaskVal == 0.0f)
                    || !_processChannelSet.contains(z)
                    )
                {
                    outData = inData;
                    // ++inData;
                    // ++outData;
                    continue;
                }

                inputVal = inData;
                if (_unpremult)
                    inputVal /= alpha;

                wrappedPerChannel(inputVal, perSampleData, z, outData, sampleColor);

                float mask = _mix;
                mask *= sideMaskVal;
                mask *= deepMaskVal;

                outData = outData * mask + inputVal * (1.0f - mask);

                if (_unpremult)
                    outData *= alpha;

                // ++inData;
                // ++outData;
            }
        }
    }

    // inPlaceOutPlane.reviseSamples();
    mFnAssert(inPlaceOutPlane.isComplete());
    deepOutPlane = inPlaceOutPlane;
    return true;
}

void DeepCWrapper::top_knobs(Knob_Callback f)
{
    Input_ChannelSet_knob(f, &_processChannelSet, 0, "channels");
    Bool_knob(f, &_unpremult, "unpremult", "(un)premult by alpha");
    Tooltip(f, "Unpremultiply channels in 'channels' before grading, "
    "then premult again after. This is always by rgba.alpha, as this "
    "is how Deep data is constructed. Should probably always be checked.");

    Divider(f, "");
}


void DeepCWrapper::custom_knobs(Knob_Callback f)
{
    Float_knob(f, &_gain, "gain");
}


void DeepCWrapper::bottom_knobs(Knob_Callback f)
{
    Divider(f, "");

    Input_Channel_knob(f, &_deepMaskChannel, 1, 0, "deep_mask", "deep input mask");
    Tooltip(f, "Use an existing channel in your deep stream based on each sample.");
    Bool_knob(f, &_invertDeepMask, "invert_deep_mask", "invert");
    Tooltip(f, "Invert the use of the deep mask channel.");
    Bool_knob(f, &_unpremultDeepMask, "unpremult_deep_mask", "unpremult");
    Tooltip(f, "Unpremult the deep mask prior to applying it.");

    Input_Channel_knob(f, &_sideMaskChannel, 1, 1, "side_mask", "side mask");
    Tooltip(f, "Use a 2d mask connected as 2d nodes on the right.");
    Bool_knob(f, &_invertSideMask, "invert_mask", "invert");
    Tooltip(f, "Invert the use of the side mask channel.");
    Float_knob(f, &_mix, "mix");
    Tooltip(f, "Dissolve between the original Image at 0 and the fulll effect at 1.");

}


void DeepCWrapper::knobs(Knob_Callback f)
{
    top_knobs(f);

    custom_knobs(f);

    bottom_knobs(f);
}


int DeepCWrapper::knob_changed(DD::Image::Knob* k)
{
    if (k->is("inputChange"))
    {
        // test input 1
        bool input1_connected = dynamic_cast<Iop*>(input(1)) != 0;
        if (!input1_connected)
        {
            _rememberedMaskChannel = _sideMaskChannel;
            knob("side_mask")->set_value(Chan_Black);
        } else
        {

            if (_rememberedMaskChannel == Chan_Black)
                { knob("side_mask")->set_value(Chan_Alpha); }
            else
                { knob("side_mask")->set_value(_rememberedMaskChannel); }
        }
        return 1;
    }

    return DeepFilterOp::knob_changed(k);
}


bool DeepCWrapper::test_input(int input, Op *op)  const
{
    switch (input)
    {
        case 0:
            return DeepFilterOp::test_input(input, op);
        case 1:
            return dynamic_cast<Iop*>(op) != 0;
    }
}


Op* DeepCWrapper::default_input(int input) const
{
    switch (input)
    {
        case 0:
            return DeepFilterOp::default_input(input);
         case 1:
             Black* dummy;
             return dynamic_cast<Op*>(dummy);
    }
}


const char* DeepCWrapper::input_label(int input, char* buffer) const
{
    switch (input)
    {
        case 0: return "";
        case 1: return "mask";
    }
}


const char* DeepCWrapper::node_help() const
{
    return
    "Like NukeWrapper, but for Deep nodes."
    "Should never be used directly, exists "
    "to be inherited from.";
}

static Op* build(Node* node) { return new DeepCWrapper(node); }
const Op::Description DeepCWrapper::d("DeepCWrapper", 0, build);
