#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/RGB.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"
#include "DDImage/Matrix4.h"

#include <stdio.h>

using namespace DD::Image;


static const char* const shapeNames[] = { "sphere", "cube", 0 };
static const char* const falloffTypeNames[] = { "linear", "smooth", 0 };

class DeepCPMatteGrade : public DeepFilterOp
{
    // values knobs write into go here
    ChannelSet process_channelset;
    ChannelSet auxiliaryChannelSet;
    bool _unpremultRGB;
    bool _unpremultPosition;

    float _positionPick[2];
    Matrix4 _axisKnob;


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

    public:

        DeepCPMatteGrade(Node* node) : DeepFilterOp(node),
            process_channelset(Mask_RGB),
            auxiliaryChannelSet(Chan_Black),
            _unpremultRGB(true),
            _unpremultPosition(false),
            _shape(0),
            _falloffType(0),
            _falloff(1.0f),
            _falloffGamma(1.0f),
            _mix(1.0f)
        {
            // defaults mostly go here
            _positionPick[0] = 0.0f;
            _positionPick[1] = 0.0f;
            
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

        void _validate(bool);
        
        virtual bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane);

        virtual void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests);
        
        virtual void knobs(Knob_Callback);
        
        virtual int knob_changed(DD::Image::Knob* k);
        
        // need to understand these better
        static const Iop::Description d;
        const char* Class() const { return d.name; }

        virtual Op* op()
        {
            return this;
        }

        const char* node_help() const;
};


void DeepCPMatteGrade::_validate(bool for_real) {
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
    
    DeepFilterOp::_validate(for_real);

    DD::Image::ChannelSet outputChannels = _deepInfo.channels();
    outputChannels += auxiliaryChannelSet;
    _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), outputChannels);
}

void DeepCPMatteGrade::getDeepRequests(Box bbox, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests) {
    if (!input0())
        return;
    DD::Image::ChannelSet get_channels = channels;
    get_channels += auxiliaryChannelSet;
    requests.push_back(RequestData(input0(), bbox, get_channels, count));
}

bool DeepCPMatteGrade::doDeepEngine(
    Box bbox,
    const DD::Image::ChannelSet& outputChannels,
    DeepOutputPlane& deep_out_plane
    )
{
    if (!input0())
        return true;

    deep_out_plane = DeepOutputPlane(outputChannels, bbox);

    DD::Image::ChannelSet get_channels = outputChannels;
    get_channels += auxiliaryChannelSet;

    DeepPlane deepInPlane;
    if (!input0()->deepEngine(bbox, get_channels, deepInPlane))
        return false;

    const DD::Image::ChannelSet inputChannels = input0()->deepInfo().channels();

    const int nOutputChans = outputChannels.size();
    
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

            // generate mask
            float m;
            Vector4 position;
            float distance;
            
            foreach(z, auxiliaryChannelSet) {
                if (colourIndex(z) >= 3)
                {
                    continue;
                }
                if (inputChannels.contains(z))
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
            if (_falloffType == 0)
            {
                m = clamp(distance, 0.0f, 1.0f);
            } else if (_falloffType == 1)
            {
                m = smoothstep(0.0f, 1.0f, distance);
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
                if (_unpremultRGB)
                    input_val /= alpha;

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
                    deepOutputPixel.push_back(output_val);
                } else {
                    output_val = output_val * _mix + input_val * (1.0f - _mix);
                    deepOutputPixel.push_back(output_val);
                } // we checked for mix == 0 earlier
            }
        }
        // Add to output plane:
        deep_out_plane.addPixel(deepOutputPixel);
    }
    return true;
}

void DeepCPMatteGrade::knobs(Knob_Callback f)
{
    ChannelSet_knob(f, &process_channelset, 0, "channels");
    ChannelSet_knob(f, &auxiliaryChannelSet, 0, "position_data");
    SetFlags(f, Knob::NO_ALPHA_PULLDOWN);
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
    // XY_knob(f, &_positionPick, "position_picker");
    Axis_knob(f, &_axisKnob, "selection");
    SetFlags(f, Knob::ALWAYS_SAVE);
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
/*
int DeepCPMatteGrade::knob_changed(DD::Image::Knob* k)
{
    if(k->is("position_picker"))
    {
        if (!input0())
            return true;
        
        Vector2 image_position;
        image_position[0] = knob("position_picker")->get_value(0);
        image_position[1] = knob("position_picker")->get_value(1);
        Box bbox((int)image_position[0],
                 (int)image_position[1],
                 (int)image_position[0] + 1,
                 (int)image_position[1] + 1);
        
        
        DeepPlane deepInPlane;
        if (!input0()->deepEngine(bbox, auxiliaryChannelSet, deepInPlane))
            return false;

        const DD::Image::ChannelSet inputChannels = input0()->deepInfo().channels();
        DeepPixel deepInPixel = deepInPlane.getPixel((int)image_position[1], (int)image_position[0]);
        
        Vector3 position;
        float distance;
        
        foreach(z, auxiliaryChannelSet) {
            if (colourIndex(z) >= 3)
            {
                continue;
            }
            if (inputChannels.contains(z))
            {
                // grab data from auxiliary channelset
                if (_unpremultPosition)
                {
                    // TODO: get alpha as well
                    position[colourIndex(z)] = deepInPixel.getOrderedSample(0, z) / 1.0f;
                } else {
                    position[colourIndex(z)] = deepInPixel.getOrderedSample(0, z);
                }
            }
        }
        knob("translate")->set_value(position[0], 0);
        knob("translate")->set_value(position[1], 1);
        knob("translate")->set_value(position[2], 2);
        
        return 1;
    }

    return DeepFilterOp::knob_changed(k);
}
*/
const char* DeepCPMatteGrade::node_help() const
{
    return
    "Mask a grade op by a world-position pass.";
}

static Op* build(Node* node) { return new DeepCPMatteGrade(node); }
const Op::Description DeepCPMatteGrade::d("DeepCPMatteGrade", 0, build);
