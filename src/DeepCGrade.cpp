#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/RGB.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"
#include "DDImage/Row.h"
#include "DDImage/Black.h"

#include <stdio.h>

using namespace DD::Image;

class DeepCGrade : public DeepFilterOp
{
    // values knobs write into go here
    ChannelSet _processChannelSet;


    // grading
    float blackpoint[3];
    float whitepoint[3];
    float black[3];
    float white[3];
    float multiply[3];
    float add[3];
    float gamma[3];

    //
    bool _reverse;
    bool _blackClamp;
    bool _whiteClamp;

    // masking
    bool _unpremult;
    Channel _maskChannel;
    Channel _rememberedMaskChannel;
    ChannelSet _maskChannelSet;
    Iop* _maskOp;
    bool _doMask;
    bool _invertMask;
    float _mix;

    public:

        DeepCGrade(Node* node) : DeepFilterOp(node),
            _processChannelSet(Mask_RGB),
            _maskChannel(Chan_Black),
            _rememberedMaskChannel(Chan_Black),
            _maskChannelSet(Chan_Black),
            _reverse(false),
            _blackClamp(false),
            _whiteClamp(false),
            _unpremult(true),
            _doMask(false),
            _invertMask(false),
            _mix(1.0f)
        {
            for (int i=0; i<3; i++)
            {
                blackpoint[i] = 0.0f;
                whitepoint[i] = 1.0f;
                black[i] = 0.0f;
                white[i] = 1.0f;
                multiply[i] = 1.0f;
                add[i] = 0.0f;
                gamma[i] = 1.0f;
            }
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


void DeepCGrade::_validate(bool for_real) {
    // make safe the gamma values
    for (int i = 0; i < 3; i++) {
        gamma[i] = clamp(gamma[i], 0.00001f, 65500.0f);
    }

    // make safe the mix
    _mix = clamp(_mix, 0.0f, 1.0f);

    _maskChannelSet = _maskChannel;

    _maskOp = dynamic_cast<Iop*>(Op::input(1));
    if (_maskOp != NULL && _maskChannel != Chan_Black)
    {
        _maskOp->validate(for_real);
        _doMask = true;
    } else {
        _doMask = false;
    }
    std::cout << "_doMask is " << _doMask << "\n";

    DeepFilterOp::_validate(for_real);

    // DD::Image::ChannelSet outputChannels = _deepInfo.channels();
    // outputChannels += _auxiliaryChannelSet;
    // outputChannels += _maskChannelSet;
    // outputChannels += Chan_DeepBack;
    // outputChannels += Chan_DeepFront;
    // _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), outputChannels);
}


bool DeepCGrade::test_input(int input, Op *op)  const
{
    switch (input)
    {
        case 0:
            return DeepFilterOp::test_input(input, op);
        case 1:
            return dynamic_cast<Iop*>(op) != 0;
    }
}


Op* DeepCGrade::default_input(int input) const
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


const char* DeepCGrade::input_label(int input, char* buffer) const
{
    switch (input)
    {
        case 0: return "";
        case 1: return "mask";
    }
}

void DeepCGrade::getDeepRequests(Box bbox, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests)
{
    if (!input0())
        return;
    DD::Image::ChannelSet get_channels = channels;
    get_channels += Chan_DeepBack;
    get_channels += Chan_DeepFront;
    requests.push_back(RequestData(input0(), bbox, get_channels, count));

    if (_doMask)
        _maskOp->request(bbox, _maskChannelSet, count);
}

bool DeepCGrade::doDeepEngine(
    Box bbox,
    const DD::Image::ChannelSet& outputChannels,
    DeepOutputPlane& deep_out_plane
    )
{
    if (!input0())
        return true;

    deep_out_plane = DeepOutputPlane(outputChannels, bbox);

    DD::Image::ChannelSet get_channels = outputChannels;

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

    if (_doMask)
    {
        _maskOp->get(bbox.y(), bbox.x(), bbox.r(), _maskChannelSet, maskRow);
        inptr = maskRow[_maskChannel] + bbox.x();
        currentYRow = bbox.y();
    }

    // for calculating the grade
    float A;
    float B;
    float G;
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

        DeepOutPixel deepOutputPixel;
        deepOutputPixel.reserve(nSamples*nOutputChans);

        if (_doMask)
        {
            if (currentYRow != it.y)
            {
                // we have not already gotten this row, get it now
                _maskOp->get(it.y, bbox.x(), bbox.r(), _maskChannelSet, maskRow);
                inptr = maskRow[_maskChannel] + bbox.x();
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
            if (inputChannels.contains(Chan_Alpha))
            {
                alpha = deepInPixel.getUnorderedSample(sampleNo, Chan_Alpha);
            } else
            {
                alpha = 1.0f;
            }

            // process the sample
            float input_val;
            float inside_val;
            float outside_val;
            float output_val;
            // for each channel
            foreach(z, outputChannels)
            {
                // channels we know we should ignore
                if (
                       z == Chan_Z
                    || z == Chan_Alpha
                    || z == Chan_DeepFront
                    || z == Chan_DeepBack
                    )
                {
                    deepOutputPixel.push_back(deepInPixel.getUnorderedSample(sampleNo, z));
                    continue;
                }

                // bail if mix is 0
                if (_mix == 0.0f)
                {
                    deepOutputPixel.push_back(deepInPixel.getUnorderedSample(sampleNo, z));
                    continue;
                }

                // only operate on rgb, i.e. first three channels
                if (colourIndex(z) >= 3)
                {
                    deepOutputPixel.push_back(deepInPixel.getUnorderedSample(sampleNo, z));
                    continue;
                }

                // do the thing, zhu-lee
                input_val = deepInPixel.getUnorderedSample(sampleNo, z);
                if (_unpremult)
                    input_val /= alpha;

                // per the NDK example for the Nuke grade node
                // this isn't how a grade should work, but it's
                // familiar to Nuke users
                // A = multiply * (gain-lift)/(whitepoint-blackpoint)
                // B = offset + lift - A*blackpoint
                // output = pow(A*input + B, 1/gamma)
                A = multiply[colourIndex(z)] * ((white[colourIndex(z)] - black[colourIndex(z)]) / (whitepoint[colourIndex(z)] - blackpoint[colourIndex(z)]));
                B = add[colourIndex(z)] + black[colourIndex(z)] - A * blackpoint[colourIndex(z)];
                G = 1.0f / gamma[colourIndex(z)];
                if (_reverse)
                {
                    // opposite gamma
                    input_val = pow(input_val, 1.0f / G);
                    // opposite linear ramp
                    if (A)
                    {
                        A = 1.0f / A;
                    } else
                    {
                        A = 1.0f;
                    }
                    B = -B * A;
                    output_val = A * input_val + B;
                } else
                {
                    output_val = pow(A * input_val + B, G);
                }

                if (_blackClamp)
                    output_val = MAX(output_val, 0.0f);

                if (_whiteClamp)
                    output_val = MIN(output_val, 1.0f);

                // we checked for mix == 0 earlier, only need to handle non-1
                if (_mix < 1.0f)
                    output_val = output_val * _mix + input_val * (1.0f - _mix);

                if (_doMask)
                    output_val = output_val * sideMaskVal + input_val * (1.0f - sideMaskVal);

                if (_unpremult)
                    output_val = output_val * alpha;

                deepOutputPixel.push_back(output_val);
            }
        }
        // Add to output plane:
        deep_out_plane.addPixel(deepOutputPixel);
    }
    return true;
}

void DeepCGrade::knobs(Knob_Callback f)
{
    Input_ChannelSet_knob(f, &_processChannelSet, 0, "channels");
    Tooltip(f, "Unpremultiply channels in 'channels' before grading, "
    "then premult again after. This is always by rgba.alpha, as this "
    "is how Deep data is constructed. Should probably always be checked.");

    Divider(f, "");

    Color_knob(f, blackpoint, IRange(-1, 1), "blackpoint", "blackpoint");
    Tooltip(f, "This color is turned into black");
    Color_knob(f, whitepoint, IRange(0, 4), "whitepoint", "whitepoint");
    Tooltip(f, "This color is turned into white");
    Color_knob(f, black, IRange(-1, 1), "black", "lift");
    Tooltip(f, "Black is turned into this color");
    Color_knob(f, white, IRange(0, 4), "white", "gain");
    Tooltip(f, "White is turned into this color");
    Color_knob(f, multiply, IRange(0, 4), "multiply", "multiply");
    Tooltip(f, "Constant to multiply result by");
    Color_knob(f, add, IRange(-1, 1), "add", "offset");
    Tooltip(f, "Constant to add to result (raises both black & white, unlike lift)");
    Color_knob(f, gamma, IRange(0.2, 5), "gamma", "gamma");
    Tooltip(f, "Gamma correction applied to final result");
    Bool_knob(f, &_reverse, "reverse");
    SetFlags(f, Knob::STARTLINE);
    Bool_knob(f, &_blackClamp, "black_clamp");
    Bool_knob(f, &_whiteClamp, "white_clamp");

    Divider(f, "");
    Input_Channel_knob(f, &_maskChannel, 1, 1, "mask");
    Bool_knob(f, &_invertMask, "invert_mask", "invert");
    Bool_knob(f, &_unpremult, "unpremult", "(un)premult by alpha");
    SetFlags(f, Knob::STARTLINE);
    Float_knob(f, &_mix, "mix");
}

int DeepCGrade::knob_changed(DD::Image::Knob* k)
{
    if (k->is("inputChange"))
    {
        // test input 1
        bool input1_connected = dynamic_cast<Iop*>(input(1)) != 0;
        if (!input1_connected)
        {
            _rememberedMaskChannel = _maskChannel;
            knob("mask")->set_value(Chan_Black);
        } else
        {

            if (_rememberedMaskChannel == Chan_Black)
                { knob("mask")->set_value(Chan_Alpha); }
            else
                { knob("mask")->set_value(_rememberedMaskChannel); }
        }
        return 1;
    }

    return DeepFilterOp::knob_changed(k);
}

const char* DeepCGrade::node_help() const
{
    return
    "Mask a grade op by a world-position pass.";
}

static Op* build(Node* node) { return new DeepCGrade(node); }
const Op::Description DeepCGrade::d("DeepCGrade", 0, build);
