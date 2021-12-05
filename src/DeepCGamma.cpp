#include "DeepCWrapper.h"

using namespace DD::Image;

class DeepCGamma : public DeepCWrapper
{
    float value[3];

    public:

        DeepCGamma(Node* node) : DeepCWrapper(node)
        {
            for (int i=0; i<3; i++)
            {
                value[i] = 1.0f;
            }

        }

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


/*
The guts. Do any processing on the channel value. The result will be masked
and mixed appropriately.
*/
void DeepCGamma::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData,
    Vector3& sampleColor
    )
{
    int cIndex = colourIndex(z);

    if (value[cIndex] != 1.0){
        float v = clamp(value[cIndex], 0.00001f, 65500.0f);
        outData = pow(inputVal, 1.0/v);
    }else{
        outData = inputVal;
    }
}


void DeepCGamma::custom_knobs(Knob_Callback f)
{
    Color_knob(f, value, IRange(0.2, 5), "value", "value");

}


const char* DeepCGamma::node_help() const
{
    return "Gamma node for DeepC.\n\n"
           "Falk Hofmann 11/2021";
}


static Op* build(Node* node) { return new DeepCGamma(node); }
const Op::Description DeepCGamma::d("DeepCGamma", 0, build);