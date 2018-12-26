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
    bool _unpremult;

    // grading
    float blackpoint[3];
    float whitepoint[3];
    float black[3];
    float white[3];
    float multiply[3];
    float add[3];
    float gamma[3];

    // precalculated grade coefficients
    float A[3];
    float B[3];
    float G[3];

    //
    bool _reverse;
    bool _blackClamp;
    bool _whiteClamp;

    // masking
    Channel _deepMaskChannel;
    bool _doDeepMask;
    bool _invertDeepMask;
    bool _unpremultDeepMask;

    Channel _sideMaskChannel;
    Channel _rememberedMaskChannel;
    Iop* _maskOp;
    bool _doSideMask;
    bool _invertSideMask;

    float _mix;

    ChannelSet allNeededDeepChannels;

    public:

        DeepCGrade(Node* node) : DeepFilterOp(node),
            _processChannelSet(Mask_RGB),
            _sideMaskChannel(Chan_Black),
            _rememberedMaskChannel(Chan_Black),
            _deepMaskChannel(Chan_Black),
            _reverse(false),
            _blackClamp(false),
            _whiteClamp(false),
            _unpremult(true),
            _doDeepMask(false),
            _invertDeepMask(false),
            _unpremultDeepMask(true),
            _doSideMask(false),
            _invertSideMask(false),
            _mix(1.0f),
            allNeededDeepChannels(Chan_Black)
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

            allNeededDeepChannels += _processChannelSet;
            allNeededDeepChannels += _deepMaskChannel;
            allNeededDeepChannels += Chan_Alpha;
            allNeededDeepChannels += Chan_DeepBack;
            allNeededDeepChannels += Chan_DeepFront;
        }

        void findNeededDeepChannels(ChannelSet& neededDeepChannels);
        void _validate(bool);
        virtual void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests);

        float wrappedPerSample(size_t sampleNo, float alpha, float other);
        float wrappedPerChannel(const float * inData, float * outData, float alpha, float other);

        virtual bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane);
        virtual void knobs(Knob_Callback);
        virtual int knob_changed(DD::Image::Knob* k);


        bool test_input(int n, Op *op)  const;
        Op* default_input(int input) const;
        const char* input_label(int input, char* buffer) const;
        virtual int minimum_inputs() const { return 2; }
        virtual int maximum_inputs() const { return 2; }
        virtual int optional_input() const { return 1; }
        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};


void DeepCGrade::findNeededDeepChannels(ChannelSet& neededDeepChannels)
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

    findNeededDeepChannels(allNeededDeepChannels);

    DeepFilterOp::_validate(for_real);
}


void DeepCGrade::getDeepRequests(Box bbox, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests)
{
    if (!input0())
        return;
    DD::Image::ChannelSet requestChannels = channels;
    requestChannels += allNeededDeepChannels;
    requests.push_back(RequestData(input0(), bbox, requestChannels, count));

    if (_doSideMask)
        _maskOp->request(bbox, _sideMaskChannel, count);
}

bool DeepCGrade::doDeepEngine(
    Box bbox,
    const DD::Image::ChannelSet& requestedChannels,
    DeepOutputPlane& deepOutPlane
    )
{
    if (!input0())
        return true;

    DD::Image::ChannelSet getChannels = requestedChannels;
    getChannels += allNeededDeepChannels;

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
            deepMaskVal = 0.0f;
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
                }
            }

            const float* inData = deepInPixel.getUnorderedSample(sampleNo);
            float* outData = outPixel.getWritableUnorderedSample(sampleNo);

            // process the sample
            float input_val;
            int cIndex;
            // for each channel
            foreach(z, requestedChannels)
            {
                cIndex = colourIndex(z);

                // channels we know we should pass through
                if (
                       z == Chan_Alpha
                    || z == Chan_Z
                    || z == Chan_DeepFront
                    || z == Chan_DeepBack
                    || cIndex >= 3
                    || _mix == 0.0f
                    || (_doDeepMask && deepMaskVal == 0.0f)
                    || (_doSideMask && sideMaskVal == 0.0f)
                    || !_processChannelSet.contains(z)
                    )
                {
                    *outData = *inData;
                    ++outData;
                    ++inData;
                    continue;
                }

                // do the thing, zhu-lee
                input_val = *inData;
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
                        *outData = pow(input_val, G[cIndex]);
                    // then inverse linear ramp we have already precomputed
                    *outData = A[cIndex] * input_val + B[cIndex];
                } else
                {
                    *outData = A[cIndex] * input_val + B[cIndex];
                    if (G[cIndex] != 1.0f)
                        *outData = pow(*outData, G[cIndex]);
                }

                if (_blackClamp)
                    *outData = MAX(*outData, 0.0f);

                if (_whiteClamp)
                    *outData = MIN(*outData, 1.0f);

                float mask = 1.0f;

                if (_doSideMask)
                    mask *= sideMaskVal;

                if (_doDeepMask)
                    mask *= deepMaskVal;

                if (_doDeepMask || _doSideMask)
                    *outData = *outData * mask + input_val * (1.0f - mask);

                // we checked for mix == 0 earlier, only need to handle non-1
                if (_mix < 1.0f)
                    *outData = *outData * _mix + input_val * (1.0f - _mix);

                if (_unpremult)
                    *outData *= alpha;

                ++inData;
                ++outData;
            }
        }
    }

    // inPlaceOutPlane.reviseSamples();
    mFnAssert(inPlaceOutPlane.isComplete());
    deepOutPlane = inPlaceOutPlane;
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
