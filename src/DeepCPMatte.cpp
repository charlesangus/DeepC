#include "DDImage/DeepFilterOp.h"
#include "DDImage/DeepSample.h"
#include "DDImage/Knobs.h"
#include "DDImage/RGB.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"
#include "DDImage/Matrix4.h"
#include "DDImage/Row.h"
#include "DDImage/Black.h"

#include <stdio.h>

using namespace DD::Image;


static const char* const shapeNames[] = { "sphere", "cube", 0 };
static const char* const falloffTypeNames[] = { "smooth", "linear", 0 };
enum { SMOOTH, LINEAR };

class DeepCPMatte : public DeepFilterOp
{
    // values knobs write into go here
    ChannelSet _outputChannelSet;
    ChannelSet _auxiliaryChannelSet;
    bool _unpremultOutput;
    bool _unpremultPosition;

    float _positionPick[2];
    Matrix4 _axisKnob;

    int _shape;
    int _falloffType;
    float _falloff;
    float _falloffGamma;

    // masking
    Channel _deepMaskChannel;
    ChannelSet _deepMaskChannelSet;
    bool _doDeepMask;
    bool _invertDeepMask;
    bool _unpremultDeepMask;

    Channel _sideMaskChannel;
    Channel _rememberedMaskChannel;
    ChannelSet _sideMaskChannelSet;
    Iop* _maskOp;
    bool _doSideMask;
    bool _invertMask;
    float _mix;

    public:

        DeepCPMatte(Node* node) : DeepFilterOp(node),
            _outputChannelSet(Mask_RGB),
            _auxiliaryChannelSet(Chan_Black),
            _deepMaskChannel(Chan_Black),
            _deepMaskChannelSet(Chan_Black),
            _rememberedMaskChannel(Chan_Black),
            _sideMaskChannel(Chan_Black),
            _sideMaskChannelSet(Chan_Black),
            _doDeepMask(false),
            _invertDeepMask(false),
            _unpremultDeepMask(true),
            _unpremultOutput(false),
            _unpremultPosition(false),
            _doSideMask(false),
            _invertMask(false),
            _mix(1.0f)
        {
            // defaults mostly go here
            _positionPick[0] = _positionPick[1] = 0.0f;
            _axisKnob.makeIdentity();

            _shape = 0;
            _falloffType = 0;
            _falloff = 1.0f;
            _falloffGamma = 1.0f;
        }

        void _validate(bool);
        bool test_input(int n, Op *op)  const;
        Op* default_input(int input) const;
        const char* input_label(int input, char* buffer) const;
        virtual bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane);
        virtual void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests);
        virtual void knobs(Knob_Callback);
        virtual int knob_changed(DD::Image::Knob* k);

        virtual int minimum_inputs() const { return 2; }
        virtual int maximum_inputs() const { return 2; }
        virtual int optional_input() const { return 1; }
        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};


void DeepCPMatte::_validate(bool for_real)
{

    _falloffGamma = clamp(_falloffGamma, 0.00001f, 65500.0f);

    // make safe the mix
    _mix = clamp(_mix, 0.0f, 1.0f);

    _sideMaskChannelSet = _sideMaskChannel;

    _maskOp = dynamic_cast<Iop*>(Op::input(1));
    if (_maskOp != NULL && _sideMaskChannel != Chan_Black)
    {
        _maskOp->validate(for_real);
        _doSideMask = true;
    } else {
        _doSideMask = false;
    }

    if (_deepMaskChannel != Chan_Black)
    { _doDeepMask = true; }
    else
    { _doDeepMask = false; }

    DeepFilterOp::_validate(for_real);

    // add our output channels to the _deepInfo
    ChannelSet new_channelset;
    new_channelset = _deepInfo.channels();
    new_channelset += _outputChannelSet;
    _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), new_channelset);
}


void DeepCPMatte::getDeepRequests(Box bbox, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests)
{
    if (!input0())
        return;

    // make sure we're asking for all required channels
    ChannelSet get_channels = channels;
    get_channels += _auxiliaryChannelSet;
    get_channels += _deepMaskChannel;
    get_channels += Chan_DeepBack;
    get_channels += Chan_DeepFront;
    requests.push_back(RequestData(input0(), bbox, get_channels, count));

    // make sure to request anything we want from the Iop side, too
    if (_doSideMask)
        _maskOp->request(bbox, _sideMaskChannelSet, count);
}

bool DeepCPMatte::doDeepEngine(
    Box bbox,
    const DD::Image::ChannelSet& outputChannels,
    DeepOutputPlane& deep_out_plane
    )
{
    if (!input0())
        return true;

    deep_out_plane = DeepOutputPlane(outputChannels, bbox);

    DD::Image::ChannelSet get_channels = outputChannels;
    get_channels += _auxiliaryChannelSet;

    DeepPlane deepInPlane;
    if (!input0()->deepEngine(bbox, get_channels, deepInPlane))
        return false;

    const DD::Image::ChannelSet inputChannels = input0()->deepInfo().channels();

    const int nOutputChans = outputChannels.size();

    // mask input stuff
    const float* inptr;
    float sideMaskVal;
    int currentYRow;
    Row maskRow(bbox.x(), bbox.r());

    if (_doSideMask)
    {
        _maskOp->get(bbox.y(), bbox.x(), bbox.r(), _sideMaskChannelSet, maskRow);
        inptr = maskRow[_sideMaskChannel] + bbox.x();
        currentYRow = bbox.y();
    }

    float result;

    for (Box::iterator it = bbox.begin(); it != bbox.end(); ++it)
    {
        if (Op::aborted())
            return false; // bail fast on user-interrupt

        // Get the deep pixel from the input plane:
        DeepPixel deepInPixel = deepInPlane.getPixel(it);
        const unsigned nSamples = deepInPixel.getSampleCount();
        if (nSamples == 0) {
            deep_out_plane.addHole(); // no samples, skip it
            continue;
        }

        ChannelSet available;
        available = deepInPixel.channels();

        // create initialized, don't create and then init
        size_t outPixelSize;
        outPixelSize = nSamples * nOutputChans * sizeof(float);
        DeepOutPixel deepOutputPixel(outPixelSize);

        if (_doSideMask)
        {
            if (currentYRow != it.y)
            {
                // we have not already gotten this row, get it now
                _maskOp->get(it.y, bbox.x(), bbox.r(), _sideMaskChannelSet, maskRow);
                inptr = maskRow[_sideMaskChannel] + bbox.x();
                sideMaskVal = inptr[it.x];
                sideMaskVal = clamp(sideMaskVal);
                currentYRow = it.y;
            } else
            {
                // we've already got this row, just get the value
                sideMaskVal = inptr[it.x];
                sideMaskVal = clamp(sideMaskVal);
            }
            if (_invertMask)
                sideMaskVal = 1.0f - sideMaskVal;
        }

        // for each sample
        for (unsigned sampleNo=0; sampleNo < nSamples; ++sampleNo)
        {

            // alpha
            float alpha;
            if (available.contains(Chan_Alpha))
            {
                alpha = deepInPixel.getUnorderedSample(sampleNo, Chan_Alpha);
            } else
            {
                alpha = 1.0f;
            }

            // deep masking
            float deepMaskVal;
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
                    deepMaskVal = 1.0f;
                }
            }

            // generate mask
            float m;
            Vector4 position;
            float distance;

            foreach(z, _auxiliaryChannelSet) {
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
                    } else {
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
                distance = pow(position[0] * position[0] + position[1] * position[1] + position[2] * position[2], .5);
            } else
            {
                // cube
                distance = clamp(1-abs(position[0])) * clamp(1-abs(position[1])) * clamp(1-abs(position[2]));
            }
            distance = clamp((1 - distance) / _falloff, 0.0f, 1.0f);
            distance = pow(distance, 1.0f / _falloffGamma);

            // falloff
            if (_falloffType == SMOOTH)
            {
                m = smoothstep(0.0f, 1.0f, distance);
            } else if (_falloffType == LINEAR)
            {
                m = clamp(distance, 0.0f, 1.0f);
            }


            // process the sample
            float input_val;
            float inside_val;
            float outside_val;
            float output_val;
            int cIndex;
            // for each channel
            foreach(z, outputChannels)
            {
                cIndex = colourIndex(z);
                // passthrough channels we know we should ignore
                // passthrough all but the output channels
                // only operate on rgb, i.e. first three channels
                // bail if mix is 0
                if (
                       z == Chan_Z
                    || z == Chan_Alpha
                    || z == Chan_DeepFront
                    || z == Chan_DeepBack
                    || !_outputChannelSet.contains(z)
                    || _mix == 0.0f
                    )
                {
                    deepOutputPixel.push_back(deepInPixel.getUnorderedSample(sampleNo, z));
                    continue;
                }

                input_val = m;
                output_val = m;


                // we checked for mix == 0 earlier, only need to handle non-1
                if (_mix < 1.0f)
                    output_val *= _mix;

                float mask = 1.0f;

                if (_doSideMask)
                    mask *= sideMaskVal;

                if (_doDeepMask)
                    mask *= deepMaskVal;

                if (_doDeepMask || _doSideMask)
                    output_val = output_val * mask;

                if (!_unpremultOutput)
                    output_val *= alpha;

                deepOutputPixel.push_back(output_val);
            }
        }
        // Add to output plane:
        deep_out_plane.addPixel(deepOutputPixel);
    }
    return true;
}

void DeepCPMatte::knobs(Knob_Callback f)
{
    Input_ChannelSet_knob(f, &_auxiliaryChannelSet, 0, "position_data");
    Bool_knob(f, &_unpremultPosition, "unpremult_position_data", "Unpremult Position Data");
    Tooltip(f, "Uncheck for ScanlineRender Deep data, check for (probably) "
    "all other renderers. Nuke stores position data from the ScanlineRender "
    "node unpremultiplied, contrary to the Deep spec. Other renderers "
    "presumably store position data (and all other data) premultiplied, "
    "as required by the Deep spec.");
    Input_ChannelSet_knob(f, &_outputChannelSet, 0, "output");
    Bool_knob(f, &_unpremultOutput, "unpremult_output", "output unpremultiplied");
    Tooltip(f, "If, for some reason, you want your mask stored without "
    "premultipling it, contrary to the Deep spec. "
    "Should probably never be checked.");


    Divider(f);
    BeginGroup(f, "Position");
    Axis_knob(f, &_axisKnob, "selection");
    EndGroup(f); // Position

    Divider(f, "");
    Enumeration_knob(f, &_shape, shapeNames, "shape");
    Enumeration_knob(f, &_falloffType, falloffTypeNames, "falloffType");
    Float_knob(f, &_falloff, "falloff");
    Float_knob(f, &_falloffGamma, "falloff_gamma");

    Divider(f, "");

    Input_Channel_knob(f, &_deepMaskChannel, 1, 0, "deep_mask", "deep input mask");
    Bool_knob(f, &_invertDeepMask, "invert_deep_mask", "invert");
    Bool_knob(f, &_unpremultDeepMask, "unpremult_deep_mask", "unpremult");

    Input_Channel_knob(f, &_sideMaskChannel, 1, 1, "side_mask", "side input mask");
    Bool_knob(f, &_invertMask, "invert_mask", "invert");
    Float_knob(f, &_mix, "mix");
}


int DeepCPMatte::knob_changed(DD::Image::Knob* k)
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


bool DeepCPMatte::test_input(int input, Op *op)  const
{
    switch (input)
    {
        case 0:
            return DeepFilterOp::test_input(input, op);
        case 1:
            return dynamic_cast<Iop*>(op) != 0;
    }
}


Op* DeepCPMatte::default_input(int input) const
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


const char* DeepCPMatte::input_label(int input, char* buffer) const
{
    switch (input)
    {
        case 0: return "";
        case 1: return "mask";
    }
}


const char* DeepCPMatte::node_help() const
{
    return
    "Generate noise to use for grading operations.";
}

static Op* build(Node* node) { return new DeepCPMatte(node); }
const Op::Description DeepCPMatte::d("DeepCPMatte", 0, build);
