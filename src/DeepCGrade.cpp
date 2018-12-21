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

    // precalculate grade coefficients
    float A[3];
    float B[3];
    float G[3];

    //
    bool _reverse;
    bool _blackClamp;
    bool _whiteClamp;

    // masking
    Channel _deepMaskChannel;
    ChannelSet _deepMaskChannelSet;
    bool _doDeepMask;
    bool _invertDeepMask;
    bool _unpremultDeepMask;

    bool _unpremult;
    Channel _sideMaskChannel;
    Channel _rememberedMaskChannel;
    ChannelSet _sideMaskChannelSet;
    Iop* _maskOp;
    bool _doSideMask;
    bool _invertSideMask;

    float _mix;

    public:

        DeepCGrade(Node* node) : DeepFilterOp(node),
            _processChannelSet(Mask_RGB),
            _sideMaskChannel(Chan_Black),
            _rememberedMaskChannel(Chan_Black),
            _sideMaskChannelSet(Chan_Black),
            _deepMaskChannel(Chan_Black),
            _deepMaskChannelSet(Chan_Black),
            _reverse(false),
            _blackClamp(false),
            _whiteClamp(false),
            _unpremult(true),
            _doDeepMask(false),
            _invertDeepMask(false),
            _unpremultDeepMask(true),
            _doSideMask(false),
            _invertSideMask(false),
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


void DeepCGrade::_validate(bool for_real)
{

    for (int i = 0; i < 3; i++) {
        // make safe the gamma values
        gamma[i] = clamp(gamma[i], 0.00001f, 65500.0f);

        // for calculating the grade - precompute the coefficients
        A[i] = multiply[i] * ((white[i] - black[i]) / (whitepoint[i] - blackpoint[i]));
        B[i] = add[i] + black[i] - A[i] * blackpoint[i];
        G[i] = 1.0f / gamma[i];
        if (_reverse)
        {
            // opposite linear ramp
            if (A[i])
            {
                A[i] = 1.0f / A[i];
            } else
            {
                A[i] = 1.0f;
            }
            B[i] = -B[i] * A[i];
            // inverse gamma
            G[i] = 1.0f/G[i];
        }
    }


    // make safe the mix
    _mix = clamp(_mix, 0.0f, 1.0f);

    _sideMaskChannelSet = _sideMaskChannel;
    _deepMaskChannelSet = _deepMaskChannel;

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

    // DD::Image::ChannelSet outputChannels = _deepInfo.channels();
    // outputChannels += _auxiliaryChannelSet;
    // outputChannels += _sideMaskChannelSet;
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
    get_channels += _deepMaskChannel;
    get_channels += Chan_DeepBack;
    get_channels += Chan_DeepFront;
    requests.push_back(RequestData(input0(), bbox, get_channels, count));

    if (_doSideMask)
        _maskOp->request(bbox, _sideMaskChannelSet, count);
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
    float sideMaskVal;
    int currentYRow;
    Row maskRow(bbox.x(), bbox.r());

    if (_doSideMask)
    {
        _maskOp->get(bbox.y(), bbox.x(), bbox.r(), _sideMaskChannelSet, maskRow);
        currentYRow = bbox.y();
    }

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
                currentYRow = it.y;
            }
            sideMaskVal = maskRow[_sideMaskChannel][it.x];
            sideMaskVal = clamp(sideMaskVal);
            if (_invertSideMask)
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
                // we already have the alpha, don't get it again
                if (z == Chan_Alpha)
                {
                    deepOutputPixel.push_back(alpha);
                    continue;
                }

                // channels we know we should pass through
                if (
                       z == Chan_Z
                    || z == Chan_DeepFront
                    || z == Chan_DeepBack
                    || cIndex >= 3
                    || _mix == 0.0f
                    || (_doDeepMask && deepMaskVal == 0.0f)
                    || (_doSideMask && sideMaskVal == 0.0f)
                    || !_processChannelSet.contains(z)
                    )
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
                // these are precomputed and reversed if necessary in
                // _validate()

                if (_reverse)
                {
                    // opposite gamma, precomputed
                    if (G[cIndex] != 1.0f)
                        input_val = pow(input_val, G[cIndex]);
                    // then inverse linear ramp we have already precomputed
                    output_val = A[cIndex] * input_val + B[cIndex];
                } else
                {
                    output_val = A[cIndex] * input_val + B[cIndex];
                    if (G[cIndex] != 1.0f)
                        output_val = pow(output_val, G[cIndex]);
                }

                if (_blackClamp)
                    output_val = MAX(output_val, 0.0f);

                if (_whiteClamp)
                    output_val = MIN(output_val, 1.0f);

                // we checked for mix == 0 earlier, only need to handle non-1
                if (_mix < 1.0f)
                    output_val = output_val * _mix + input_val * (1.0f - _mix);

                float mask = 1.0f;

                if (_doSideMask)
                    mask *= sideMaskVal;

                if (_doDeepMask)
                    mask *= deepMaskVal;

                if (_doDeepMask || _doSideMask)
                    output_val = output_val * mask + input_val * (1.0f - mask);

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
    Bool_knob(f, &_unpremult, "unpremult", "(un)premult by alpha");
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

    Input_Channel_knob(f, &_deepMaskChannel, 1, 0, "deep_mask", "deep input mask");
    Bool_knob(f, &_invertDeepMask, "invert_deep_mask", "invert");
    Bool_knob(f, &_unpremultDeepMask, "unpremult_deep_mask", "unpremult");

    Input_Channel_knob(f, &_sideMaskChannel, 1, 1, "side_mask");
    Bool_knob(f, &_invertSideMask, "invert_mask", "invert");
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

const char* DeepCGrade::node_help() const
{
    return
    "Deep Grade with inline Deep masking and side-input"
    "flat-image masking. Compatible with the regular "
    "Grade node, i.e. gives same output for same "
    "knob settings and input, except works in Deep.";
}

static Op* build(Node* node) { return new DeepCGrade(node); }
const Op::Description DeepCGrade::d("DeepCGrade", 0, build);
