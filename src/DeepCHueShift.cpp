#include "DeepCWrapper.h"
#include "DDImage/RGB.h"
#include <DDImage/Convolve.h>

#include <math.h>

using namespace DD::Image;
using namespace std;

class DeepCHueShift : public DeepCWrapper
{
    Matrix3 mtx_rgb_to_yiq;
    Matrix3 mtx_yiq_to_rgb;
    Matrix3 mtx_hsv;
    ChannelSet _brothers;

    double _hue;
    double _saturation;
    double _value;

public:
    DeepCHueShift(Node *node) : DeepCWrapper(node)
    {
        _hue = 0.0f;
        _saturation = _value = 1.0f;
    }

        void findNeededDeepChannels(ChannelSet& neededDeepChannels);
        
        virtual void wrappedPerSample(
            Box::iterator it,
            size_t sampleNo,
            float alpha,
            DeepPixel deepInPixel,
            float &perSampleData,
            Vector3 &sampleColor
            );

        virtual void wrappedPerChannel(
            const float inputVal,
            float perSampleData,
            Channel z,
            float& outData,
            Vector3& sampleColor
            );


        void _validate(bool);
        virtual void custom_knobs(Knob_Callback f);

        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};


void DeepCHueShift::findNeededDeepChannels(ChannelSet& neededDeepChannels)
{
    DeepCWrapper::findNeededDeepChannels(neededDeepChannels);
    Channel firstChan;
    firstChan = _processChannelSet.first();
    _brothers = Chan_Black;
    _brothers.addBrothers(firstChan, 3);
    neededDeepChannels += _brothers;
}

void DeepCHueShift::_validate(bool for_real){

    DeepCWrapper::_validate(for_real);

    mtx_rgb_to_yiq.set(0.299f,    0.587f,  0.114f,
                       0.596f,   -0.274f, -0.321f,
                       0.211f,   -0.523f,  0.311f);

    mtx_yiq_to_rgb = mtx_rgb_to_yiq.inverse();

    double phi = _hue * M_PI / (double)180;
    double u = cos(phi);
    double w = sin(phi);

    mtx_hsv.set(_value,              0.0f,                      0.0f,
                 0.0f,     _value * _saturation * u,  -(_value * _saturation * w),
                 0.0f,     _value * _saturation * w,    _value * _saturation * u);
}
void DeepCHueShift::wrappedPerSample(
    Box::iterator it,
    size_t sampleNo,
    float alpha,
    DeepPixel deepInPixel,
    float &perSampleData,
    Vector3 &sampleColor
    )
{
    ChannelSet available;
    available = deepInPixel.channels();

    foreach(z, _brothers) {
        int cIndex = colourIndex(z);
        if (cIndex > 3)
        {
            continue;
        }
        if (available.contains(z))
        {
            sampleColor[cIndex] = deepInPixel.getUnorderedSample(sampleNo, z) / alpha;
        }
     }

    sampleColor = mtx_rgb_to_yiq.transform(sampleColor);
    sampleColor = mtx_hsv.transform(sampleColor);
    sampleColor = mtx_yiq_to_rgb.transform(sampleColor);
}

void DeepCHueShift::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData,
    Vector3& sampleColor
    )
{
    int cIndex = colourIndex(z);
    outData = sampleColor[cIndex];
}


void DeepCHueShift::custom_knobs(Knob_Callback f)
{
    Double_knob(f, &_hue, IRange(-180, 180), "hue", "hue");
    Tooltip(f, "Rotate the hue of the image. The value is in degree.");
    Double_knob(f, &_saturation, "saturation", "saturation");
    Tooltip(f, "Scale the saturation only.");
    Double_knob(f, &_value, "value", "value");
    Tooltip(f, "Scale the brightness only.");
}

const char* DeepCHueShift::node_help() const
{
    return "A Deep HueShift node.\n"
           "Rotate Hue and scale Saturation/Brightness individually.\n\n"
           "Falk Hofmann 12/2021";
}


static Op* build(Node* node) { return new DeepCHueShift(node); }
const Op::Description DeepCHueShift::d("DeepCHueShift", 0, build);
