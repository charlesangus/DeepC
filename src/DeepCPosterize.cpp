#include "DeepCWrapper.h"

using namespace DD::Image;

class DeepCPosterize : public DeepCWrapper
{
    double colors;
    double _save_colors;

public:
    DeepCPosterize(Node *node) : DeepCWrapper(node)
    {
        for (int i = 0; i < 3; i++)
        {
            colors = 10.0f;
            _save_colors = 10.0f;
        }
        }

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

void DeepCPosterize::_validate(bool for_real)
{
    _save_colors = (colors < 1) ? 1 : colors;
    DeepCWrapper::_validate(for_real);
}

void DeepCPosterize::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData,
    Vector3& sampleColor
    )
{
    int val = inputVal * _save_colors;
    outData = val / _save_colors;
}


void DeepCPosterize::custom_knobs(Knob_Callback f)
{
    Double_knob(f, &colors, IRange(2, 256), "colors", "Colors");
    Tooltip(f, "Define amount of colors to keep.");
}

const char* DeepCPosterize::node_help() const
{
    return "Posterize node for DeepC.\n\n"
           "Falk Hofmann 12/2021";
}


static Op* build(Node* node) { return new DeepCPosterize(node); }
const Op::Description DeepCPosterize::d("DeepCPosterize", 0, build);