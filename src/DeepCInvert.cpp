#include "DeepCWrapper.h"

using namespace DD::Image;

class DeepCInvert : public DeepCWrapper
{
    bool _clamp;

    public:

        DeepCInvert(Node* node) : DeepCWrapper(node)
        {
            _clamp = false;

        }

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

void DeepCInvert::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData
    )
{
    int cIndex = colourIndex(z);
    outData = 1 - inputVal;

    if (_clamp){
        outData = clamp(outData, 0.0, 1.0);
    }

}


void DeepCInvert::custom_knobs(Knob_Callback f)
{
    Bool_knob(f, &_clamp, "clamp", "clamp");

}


const char* DeepCInvert::node_help() const
{
    return "Invert node for DeepC.\n"
           "Falk Hofmann 11/2021";
}


static Op* build(Node* node) { return new DeepCInvert(node); }
const Op::Description DeepCInvert::d("DeepCInvert", 0, build);