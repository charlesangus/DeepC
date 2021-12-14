/* Constant node for deep nodes with adjustable samples, depth range and front/back colors.

Falk Hofmann, 12/2021
*/

#include "DDImage/Iop.h"
#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"

static const char* CLASS = "DeepCConstant";
static const char* const enumAlphaTypes[] = { "uniform", "additive", "multiplicative", 0 };

using namespace DD::Image;

class DeepCConstant : public DeepFilterOp
{
    FormatPair formats;
    ChannelSet channels;
    float color_front[4];
    float color_back[4];
    double _front;
    double _back;
    int _samples;
    float _overallDepth;
    float _sampleDistance;
    Vector4 _values_front;
    Vector4 _values_back;
    int _alphaType;

public:

    int minimum_inputs() const { return 0; }

    DeepCConstant(Node* node) : DeepFilterOp(node) {

        formats.format(nullptr);
        _front = 0;
        _back = 10;
        _samples = 1;
        _overallDepth = 1;
        _sampleDistance = 1.0f;
        channels = Mask_RGBA;
        _alphaType = 2;
        for (int n = 0; n < 4; n++) {
            _values_front[n] = _values_back[n] = 0.0f;
            color_front[n] = 0.2f;
            color_back[n] = 0.5f;
        }

    }
    void _validate(bool);
    bool doDeepEngine(Box bbox, const ChannelSet& requestedChannels, DeepOutputPlane& deepOutPlane);
    void knobs(Knob_Callback);
    int knob_changed(Knob* k);

    void calcValues(Knob* colorKnob, Vector4& color);

    static const Iop::Description d;
    const char* Class() const { return d.name; }
    const char* node_help() const;
    virtual Op* op() { return this; }
};

const char* DeepCConstant::node_help() const
{
    return "A DeepConstant with defined deep range, amount of samples\n"
        "and color for front and back.\n\n"
        "Falk Hofmann 12/2021";
}

void DeepCConstant::knobs(Knob_Callback f)
{
    Input_ChannelSet_knob(f, &channels, 4, "channels");
    AColor_knob(f, color_front, "color_front", "color front");
    SetFlags(f, Knob::MAGNITUDE);
    SetFlags(f, Knob::SLIDER);
    Tooltip(f, "Define color for the very front sample. Towards back, colors will be linear interpolated.");
    AColor_knob(f, color_back, "color_back", "color back");
    Tooltip(f, "Define color for the very back sample. Towards front, colors will be linear interpolated.");
    Format_knob(f, &formats, "format", "format");
    Divider(f, "");
    Enumeration_knob(f, &_alphaType, enumAlphaTypes, "alpha_mode", "split alpha mode");
    Tooltip(f, "Select the alpha mode.\n\n"
        "multiplactive will match the image visually as compared to a 2d constant.\n\n"
        "additive does a straight division by the number of samples and therefore will not match the 2d equivalent.\n\n"
        "uniform applies the given color values straight to the samples, without considering and modifications due deep sample addition.");
    Double_knob(f, &_front, "front", "front");
    Tooltip(f, "Closest distance from camera.");
    Double_knob(f, &_back, "back", "back");
    Tooltip(f, "Farthest distance from camera.");
    Int_knob(f, &_samples, "samples", "samples");
    Tooltip(f, "Amount of deep samples per pixel.");
    SetRange(f, 2, 1000);
    SetFlags(f, Knob::NO_ANIMATION);

}

void DeepCConstant::calcValues(Knob* colorKnob, Vector4& color) {

    float a = colorKnob->get_value(colourIndex(Chan_Alpha));
    int saveSample = (_samples > 0) ? _samples : 1;

    for (int c = 0; c < 4; c++) {
        float val = colorKnob->get_value(c);
        switch (_alphaType)
        {
        case 0:
            color[c] = val;
            break;
        case 1:
            color[c] = val / saveSample;
            break;
        case 2:
            color[c] = (val / a) * (1.0f - pow(1.0f - a, (1.0f / saveSample)));
        }
    }
}


int DeepCConstant::knob_changed(Knob* k) {
    if (k->name() == "alpha_mode") {
        DeepCConstant::calcValues(knob("color_front"), _values_front);
        DeepCConstant::calcValues(knob("color_back"), _values_back);
    }
    return DeepFilterOp::knob_changed(k);
}

void DeepCConstant::_validate(bool for_real)
{
    ChannelSet new_channelset;
    new_channelset += Mask_Deep;
    new_channelset += Mask_Z;
    new_channelset += Mask_Alpha;
    new_channelset += channels;

    Box box(0, 0, formats.format()->width(), formats.format()->height());
    _deepInfo = DeepInfo(formats, box, new_channelset);
    _overallDepth = _back - _front;
    _sampleDistance = _overallDepth / (_samples);
    DeepCConstant::calcValues(knob("color_front"), _values_front);
    DeepCConstant::calcValues(knob("color_back"), _values_back);
}

bool DeepCConstant::doDeepEngine(DD::Image::Box box, const DD::Image::ChannelSet& channels, DeepOutputPlane& plane)
{
    int saveSample = (_samples > 0) ? _samples : 1;
    _values_front[3] = (_values_front[3] <= 0.0) ? 0.000001 : _values_front[3];
    _values_back[3] = (_values_back[3] <= 0.0) ? 0.000001 : _values_back[3];

    DeepInPlaceOutputPlane outPlane(channels, box, DeepPixel::eZDescending);
    outPlane.reserveSamples(box.area());

    for (Box::iterator it = box.begin();
        it != box.end();
        it++) {

        outPlane.setSampleCount(it, saveSample);
        DeepOutputPixel out = outPlane.getPixel(it);
        for (int sampleNo = 0; sampleNo < saveSample; sampleNo++) {

            float depth = _sampleDistance * sampleNo;
            float weight = (1.0f, 0.0f, depth / _overallDepth);

            foreach(z, channels)
            {
                float& output = out.getWritableOrderedSample(sampleNo, z);
                int cIndex = colourIndex(z);

                if (z == Chan_DeepFront) {
                    output = _front + depth;

                }
                else if (z == Chan_DeepBack) {
                    output = _front + depth + _sampleDistance;

                }
                else {
                    output = (weight * _values_back[cIndex]) + ((1 - weight) * _values_front[cIndex]);
                }
            }
        }
    }
    mFnAssert(outPlane.isComplete());
    plane = outPlane;


    return true;
}

static Op* build(Node* node) { return new DeepCConstant(node); }
const Op::Description DeepCConstant::d("DeepCConstant", 0, build);
