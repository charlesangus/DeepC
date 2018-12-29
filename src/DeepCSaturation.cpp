#include "DeepCWrapper.h"
#include "DDImage/RGB.h"

using namespace DD::Image;

class DeepCSaturation : public DeepCWrapper
{

    // grading
    float _saturation;

    ChannelSet _brothers;

    public:

        DeepCSaturation(Node* node) : DeepCWrapper(node)
        {
            _saturation = 1.0f;
            _brothers = Chan_Black;
        }

        void findNeededDeepChannels(ChannelSet& neededDeepChannels);

        virtual void wrappedPerSample(
            Box::iterator it,
            size_t sampleNo,
            float alpha,
            DeepPixel deepInPixel,
            float &perSampleData
            );
        virtual void wrappedPerChannel(
            const float inputVal,
            float perSampleData,
            Channel z,
            float& outData
            );

        virtual void custom_knobs(Knob_Callback f);

        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};

void DeepCSaturation::findNeededDeepChannels(ChannelSet& neededDeepChannels)
{
    DeepCWrapper::findNeededDeepChannels(neededDeepChannels);
    Channel firstChan;
    firstChan = _processChannelSet.first();
    _brothers = Chan_Black;
    _brothers.addBrothers(firstChan, 3);
    neededDeepChannels += _brothers;
}

void DeepCSaturation::wrappedPerSample(
    Box::iterator it,
    size_t sampleNo,
    float alpha,
    DeepPixel deepInPixel,
    float &perSampleData
    )
{
    ChannelSet available;
    available = deepInPixel.channels();

    float rgb[3];
    rgb[0] = rgb[1] = rgb[2] = 0.0f;

    foreach(z, _brothers) {
        if (colourIndex(z) >= 3)
        {
            continue;
        }
        if (available.contains(z))
        {
            rgb[colourIndex(z)] = deepInPixel.getUnorderedSample(sampleNo, z) / alpha;
        }
    }
    perSampleData = y_convert_rec709(rgb[0], rgb[1], rgb[2]);
}


void DeepCSaturation::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData
    )
{
    outData = inputVal * _saturation + perSampleData * (1.0f - _saturation);
}


void DeepCSaturation::custom_knobs(Knob_Callback f)
{
    Float_knob(f, &_saturation, IRange(0, 4), "saturation");
}


const char* DeepCSaturation::node_help() const
{
    return
    "Saturation node for DeepC.";
}


static Op* build(Node* node) { return new DeepCSaturation(node); }
const Op::Description DeepCSaturation::d("DeepCSaturation", 0, build);
