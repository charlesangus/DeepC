#include "DDImage/DeepPixelOp.h"
#include "DDImage/Iop.h"
#include "DDImage/Knobs.h"
#include "DDImage/RGB.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"
#include "DDImage/Matrix4.h"
#include "DDImage/Black.h"

#include <stdio.h>

using namespace DD::Image;


static const char* const shapeNames[] = { "sphere", "cube", 0 };
static const char* const falloffTypeNames[] = { "linear", "smooth", 0 };

class DeepCGrade : public DeepPixelOp
{
    // values knobs write into go here
    ChannelSet process_channelset;
    ChannelSet wp_channelset;
    bool _unpremultRGB;
    bool _unpremultPosition;

    Matrix4 _axisKnob;

    virtual int minimum_inputs() const { return 2; }
    virtual int maximum_inputs() const { return 2; }
    virtual int optional_input() const { return 1; }
    
    int _shape;
    int _falloffType;
    float _falloff;
    float _falloffGamma;

    float blackpointIn[3];
    float whitepointIn[3];
    float blackIn[3];
    float whiteIn[3];
    float multiplyIn[3];
    float addIn[3];
    float gammaIn[3];

    float blackpointOut[3];
    float whitepointOut[3];
    float blackOut[3];
    float whiteOut[3];
    float multiplyOut[3];
    float addOut[3];
    float gammaOut[3];

    float _mix;

    Iop* _maskOp;
    
    public:

        DeepCGrade(Node* node) : DeepPixelOp(node),
            process_channelset(Mask_RGB),
            wp_channelset(Chan_Black),
            _unpremultRGB(true),
            _unpremultPosition(false),
            _shape(0),
            _falloffType(0),
            _falloff(1.0f),
            _falloffGamma(1.0f),
            _mix(1.0f)
        {
            // defaults mostly go here
            _axisKnob.makeIdentity();

            for (int i=0; i<3; i++)
            {
                blackpointIn[i] = 0.0f;
                whitepointIn[i] = 1.0f;
                blackIn[i] = 0.0f;
                whiteIn[i] = 1.0f;
                multiplyIn[i] = 1.0f;
                addIn[i] = 0.0f;
                gammaIn[i] = 1.0f;

                blackpointOut[i] = 0.0f;
                whitepointOut[i] = 1.0f;
                blackOut[i] = 0.0f;
                whiteOut[i] = 1.0f;
                multiplyOut[i] = 1.0f;
                addOut[i] = 0.0f;
                gammaOut[i] = 1.0f;
            }
        }


        // Wrapper function to work around the "non-virtual thunk" issue on linux when symbol hiding is enabled.
        virtual bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane)
        {
            return DeepPixelOp::doDeepEngine(box, channels, plane);
        }


        // Wrapper function to work around the "non-virtual thunk" issue on linux when symbol hiding is enabled.
        virtual void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests)
        {
            DeepPixelOp::getDeepRequests(box, channels, count, requests);
        }


        // We always need the rgb no matter what channels are asked for:
        void in_channels(int, ChannelSet& channels) const
        {
            if (channels & Mask_RGB)
                channels += Mask_RGB;
            channels += process_channelset;
            channels += wp_channelset;
            channels += Chan_Alpha;
        }

        // need to understand these better
        static const Iop::Description d;
        const char* Class() const { return d.name; }


        // for menu listing and function controls
        virtual void knobs(Knob_Callback);



        virtual Op* op()
        {
            return this;
        }

        // TODO: clean
        bool test_input(int n, Op *op)  const {
            if (n == 1) {
                return dynamic_cast<Iop*>(op) != 0;
            }

            return DeepPixelOp::test_input(n, op);
        }

        // TODO: clean
        Op* default_input(int input) const {
            switch (input)
            {
                case 0:
                    return DeepPixelOp::default_input(input);
                 case 1:
                     Black* dummy;
                     return dynamic_cast<Op*>(dummy);
            }
        }


        const char* input_label(int input, char* buffer) const {
            switch (input) {
            case 1: return "mask";
            }
        }

        void _validate(bool);
        virtual void processSample(int y,
                                   int x,
                                   const DD::Image::DeepPixel& deepPixel,
                                   size_t sampleNo,
                                   const DD::Image::ChannelSet& channels,
                                   DeepOutPixel& output
                                   ) const;

        const char* node_help() const;
};


// Safe power function that does something reasonable for negative numbers
// e must be clamped to the range -MAXPOW..MAXPOW
// Retained as an option to replace normal power function later if needed
#define MAXPOW 1000.0f
static inline float P(float x, float e)
{
    return x > 0 ? powf(x, e) : x;
}

void DeepCGrade::_validate(bool for_real)
{
    // make safe the gamma values
    for (int i = 0; i < 3; i++) {
        gammaIn[i] = clamp(gammaIn[i], 0.00001f, 65500.0f);
        gammaOut[i] = clamp(gammaOut[i], 0.00001f, 65500.0f);
    }
    _falloffGamma = clamp(_falloffGamma, 0.00001f, 65500.0f);

    // make safe the falloff
    _falloff = clamp(_falloff, 0.00001f, 1.0f);

    // make safe the mix
    _mix = clamp(_mix, 0.0f, 1.0f);

    // TODO: should probably check into whether we're using an outside
    //       grade, and if not, we can optimize by not processing any
    //       pixel with m == 0.0f;

    // TODO: should check for a few degenerate states like mix=0 and
    //       optimally pass through in those cases
    _maskOp = dynamic_cast<Iop*>(Op::input(1));
    if (_maskOp != NULL)
    {
        _maskOp->validate();
    }
    
    DeepPixelOp::_validate(for_real);
}

void DeepCGrade::processSample(
    int y,
    int x,
    const DD::Image::DeepPixel& deepPixel,
    size_t sampleNo,
    const DD::Image::ChannelSet& channels,
    DeepOutPixel& output) const
{

    // alpha
    float alpha;
    alpha = deepPixel.getUnorderedSample(sampleNo, Chan_Alpha);

    // generate mask
    float m;
    Vector4 position;
    float distance;

    foreach(z, wp_channelset) {
        if (colourIndex(z) >= 3)
        {
            continue;
        }
        if (_unpremultPosition)
        {
            position[colourIndex(z)] = deepPixel.getUnorderedSample(sampleNo, z) / alpha;
        } else {
            position[colourIndex(z)] = deepPixel.getUnorderedSample(sampleNo, z);
        }
    }
    position[3] = 1.0f;
    Matrix4 inverse__axisKnob = _axisKnob.inverse();
    position = inverse__axisKnob * position;
    position = position.divide_w();

    if (_shape == 0)
    {
        // sphere
        distance = sqrt(position[0] * position[0] + position[1] * position[1] + position[2] * position[2]);
        distance = clamp((1 - distance) / _falloff, 0.0f, 1.0f);
    } else
    {
        // cube
        distance =    clamp((1.0f - fabs(position[0])) / _falloff)
                    * clamp((1.0f - fabs(position[1])) / _falloff)
                    * clamp((1.0f - fabs(position[2])) / _falloff);
    }
    distance = pow(distance, 1.0f / _falloffGamma);

    // falloff
    if (_falloffType == 0)
    {
        m = clamp(distance, 0.0f, 1.0f);
    } else if (_falloffType == 1)
    {
        m = smoothstep(0.0f, 1.0f, distance);
    }
    
    if (_maskOp != NULL) 
    {
        m  = _maskOp->at(x, y, Chan_Alpha);
    } else
    {
        m = 0.0f;
    }
    

    // process the sample
    float input_val;
    float inside_val;
    float outside_val;
    float output_val;

    foreach(z, channels) {
        // channels we know we should ignore
        if (
               z == Chan_Z
            || z == Chan_Alpha
            || z == Chan_DeepFront
            || z == Chan_DeepBack
            )
        {
            output.push_back(deepPixel.getUnorderedSample(sampleNo, z));
            continue;
        }

        // bail if mix is 0
        if (_mix == 0.0f)
        {
            output.push_back(deepPixel.getUnorderedSample(sampleNo, z));
            continue;
        }

        // is this the best way to restrict the processed channelset? maybe...
        if (process_channelset & z)
        {
            // only operate on rgb, i.e. first three channels
            if (colourIndex(z) >= 3)
            {
                output.push_back(deepPixel.getUnorderedSample(sampleNo, z));
                continue;
            }

            // do the thing, zhu-lee
            if (_unpremultRGB)
            {
                input_val = deepPixel.getUnorderedSample(sampleNo, z) / alpha;
            } else {
                input_val = deepPixel.getUnorderedSample(sampleNo, z);
            }

            // val * multiply * (gain - lift) / (whitepoint - blackpoint) + offset + lift - (multiply * (gain - lift) / (whitepoint - blackpoint)) * blackpoint, 1/gamma
            // i believe this is similar or identical to what nuke uses in a grade node, so should be familiar
            // despite not really being reasonable
            inside_val= pow(input_val * multiplyIn[colourIndex(z)] * (whiteIn[colourIndex(z)] - blackIn[colourIndex(z)]) / (whitepointIn[colourIndex(z)] - blackpointIn[colourIndex(z)]) + addIn[colourIndex(z)] + blackIn[colourIndex(z)] - (multiplyIn[colourIndex(z)] * (whiteIn[colourIndex(z)] - blackIn[colourIndex(z)]) / (whitepointIn[colourIndex(z)] - blackpointIn[colourIndex(z)])) * blackpointIn[colourIndex(z)], 1.0f/gammaIn[colourIndex(z)]);

            outside_val= pow(input_val * multiplyOut[colourIndex(z)] * (whiteOut[colourIndex(z)] - blackOut[colourIndex(z)]) / (whitepointOut[colourIndex(z)] - blackpointOut[colourIndex(z)]) + addOut[colourIndex(z)] + blackOut[colourIndex(z)] - (multiplyOut[colourIndex(z)] * (whiteOut[colourIndex(z)] - blackOut[colourIndex(z)]) / (whitepointOut[colourIndex(z)] - blackpointOut[colourIndex(z)])) * blackpointOut[colourIndex(z)], 1.0f/gammaOut[colourIndex(z)]);

            // apply mask
            output_val = (inside_val * m + outside_val * (1 - m));

            if (_unpremultRGB)
            {
                output_val = output_val * alpha;
            } // need do nothing if we're not required to premult

            if (_mix == 1.0f)
            {
                output.push_back(output_val);
            } else {
                output_val = output_val * _mix + input_val * (1 - _mix);
            } // we checked for mix == 0 earlier
        } else {
            output.push_back(deepPixel.getUnorderedSample(sampleNo, z));
            continue;
        }
    }
}

void DeepCGrade::knobs(Knob_Callback f)
{
    InputOnly_ChannelSet_knob(f, &process_channelset, 0, "channels");
    InputOnly_ChannelSet_knob(f, &wp_channelset, 0, "position_data");
    Bool_knob(f, &_unpremultRGB, "unpremult_rgb", "(Un)premult RGB");
    SetFlags(f, Knob::STARTLINE);
    Tooltip(f, "Unpremultiply channels in 'channels' before grading, \n"
    "then premult again after. This is always by rgba.alpha, as this \n"
    "is how Deep data is constructed. Should probably always be checked.");
    Bool_knob(f, &_unpremultPosition, "unpremult_position_data", "Unpremult Position Data");
    Tooltip(f, "Uncheck for ScanlineRender Deep data, check for (probably) \n"
    "all other renderers. Nuke stores position data from the ScanlineRender \n"
    "node unpremultiplied, contrary to the Deep spec. Other renderers \n"
    "presumably store position data (and all other data) premultiplied, \n"
    "as required by the Deep spec.");


    Divider(f, "PMask");
    BeginGroup(f, "PMask");
    Axis_knob(f, &_axisKnob, "selection");
    EndGroup(f);

    Divider(f, "Shape");
    Enumeration_knob(f, &_shape, shapeNames, "shape");
    Enumeration_knob(f, &_falloffType, falloffTypeNames, "falloffType");
    Float_knob(f, &_falloff, "falloff");
    Float_knob(f, &_falloffGamma, "falloff_gamma");


    Divider(f, "Grades");
    BeginClosedGroup(f, "Inside Mask Grades");

    Color_knob(f, blackpointIn, IRange(-1, 1), "blackpoint_inside", "blackpoint");
    Tooltip(f, "This color is turned into black");
    Color_knob(f, whitepointIn, IRange(0, 4), "whitepoint_inside", "whitepoint");
    Tooltip(f, "This color is turned into white");
    Color_knob(f, blackIn, IRange(-1, 1), "black_inside", "lift");
    Tooltip(f, "Black is turned into this color");
    Color_knob(f, whiteIn, IRange(0, 4), "white_inside", "gain");
    Tooltip(f, "White is turned into this color");
    Color_knob(f, multiplyIn, IRange(0, 4), "multiply_inside", "multiply");
    Tooltip(f, "Constant to multiply result by");
    Color_knob(f, addIn, IRange(-1, 1), "add_inside", "offset");
    Tooltip(f, "Constant to add to result (raises both black & white, unlike lift)");
    Color_knob(f, gammaIn, IRange(0.2, 5), "gamma_inside", "gamma");
    Tooltip(f, "Gamma correction applied to final result");

    EndGroup(f);

    BeginClosedGroup(f, "Outside Mask Grades");

    Color_knob(f, blackpointOut, IRange(-1, 1), "blackpoint_outside", "blackpoint");
    Tooltip(f, "This color is turned into black");
    Color_knob(f, whitepointOut, IRange(0, 4), "whitepoint_outside", "whitepoint");
    Tooltip(f, "This color is turned into white");
    Color_knob(f, blackOut, IRange(-1, 1), "black_outside", "lift");
    Tooltip(f, "Black is turned into this color");
    Color_knob(f, whiteOut, IRange(0, 4), "white_outside", "gain");
    Tooltip(f, "White is turned into this color");
    Color_knob(f, multiplyOut, IRange(0, 4), "multiply_outside", "multiply");
    Tooltip(f, "Constant to multiply result by");
    Color_knob(f, addOut, IRange(-1, 1), "add_outside", "offset");
    Tooltip(f, "Constant to add to result (raises both black & white, unlike lift)");
    Color_knob(f, gammaOut, IRange(0.2, 5), "gamma_outside", "gamma");
    Tooltip(f, "Gamma correction applied to final result");

    EndGroup(f);
    Divider(f, "");

    Float_knob(f, &_mix, "mix");
}

const char* DeepCGrade::node_help() const
{
    return
    "Mask a grade op by a world-position pass.";
}

static Op* build(Node* node) { return new DeepCGrade(node); }
const Op::Description DeepCGrade::d("DeepCGrade", 0, build);
