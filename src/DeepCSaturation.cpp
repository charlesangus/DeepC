#include "DeepCWrapper.h"
#include "DDImage/RGB.h"

using namespace DD::Image;


enum {
  REC709 = 0, CCIR601, AVERAGE, MAXIMUM
};

static const char* mode_names[] = {
  "Rec 709", "Ccir 601", "Average", "Maximum", nullptr
};


class DeepCSaturation : public DeepCWrapper
{

    // grading
    float _saturation;
    int _mode;

    ChannelSet _brothers;

    public:

        DeepCSaturation(Node* node) : DeepCWrapper(node)
        {
            _saturation = 1.0f;
            _mode = 0;
            _brothers = Chan_Black;
        }

        void findNeededDeepChannels(ChannelSet& neededDeepChannels);

        virtual void wrappedPerSample(
            Box::iterator it,
            size_t sampleNo,
            float alpha,
            DeepPixel deepInPixel,
            float &perSampleData,
            Vector3& sampleColor
            );
        virtual void wrappedPerChannel(
            const float inputVal,
            float perSampleData,
            Channel z,
            float& outData,
            Vector3& sampleColor
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

static inline float y_convert_ccir601(float r, float g, float b)
{
  return r * 0.299f + g * 0.587f + b * 0.114f;
}

static inline float y_convert_avg(float r, float g, float b)
{
  return (r + g + b) / 3.0f;
}

static inline float y_convert_max(float r, float g, float b)
{
  if (g > r)
    r = g;
  if (b > r)
    r = b;
  return r;
}


void DeepCSaturation::wrappedPerSample(
    Box::iterator it,
    size_t sampleNo,
    float alpha,
    DeepPixel deepInPixel,
    float &perSampleData,
    Vector3& sampleColor
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

    switch (_mode) {
        case REC709:
            perSampleData = y_convert_rec709(rgb[0], rgb[1], rgb[2]);
            break;
        case CCIR601:
            perSampleData = y_convert_ccir601(rgb[0], rgb[1], rgb[2]);
            break;
        case AVERAGE:
            perSampleData = y_convert_avg(rgb[0], rgb[1], rgb[2]);
            break;
        case MAXIMUM:
            perSampleData = y_convert_max(rgb[0], rgb[1], rgb[2]);
            break;
        default:
            perSampleData = y_convert_rec709(rgb[0], rgb[1], rgb[2]);
            break;
        }

}


void DeepCSaturation::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData,
    Vector3& sampleColor
    )
{
    outData = inputVal * _saturation + perSampleData * (1.0f - _saturation);
}


void DeepCSaturation::custom_knobs(Knob_Callback f)
{
    Float_knob(f, &_saturation, IRange(0, 4), "saturation");
    Enumeration_knob(f, &_mode, mode_names, "_mode", "luminance math");
    Tooltip(f, "Choose a mode to apply the greyscale conversion.");
}


const char* DeepCSaturation::node_help() const
{
    return
    "Saturation node for DeepC.";
}


static Op* build(Node* node) { return new DeepCSaturation(node); }
const Op::Description DeepCSaturation::d("DeepCSaturation", 0, build);
