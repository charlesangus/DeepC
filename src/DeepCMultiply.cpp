#include "DeepCWrapper.h"

using namespace DD::Image;

class DeepCMultiply : public DeepCWrapper
{
    float value[3];

    public:

        DeepCMultiply(Node* node) : DeepCWrapper(node)
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
            float& outData
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
void DeepCMultiply::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData
    )
{
    int cIndex = colourIndex(z);
    if (value[cIndex] != 1.0){
        outData = inputVal * value[cIndex];
    }else{
        outData = inputVal;
    }
}


void DeepCMultiply::custom_knobs(Knob_Callback f)
{
    Color_knob(f, value, IRange(0, 5), "value", "value");

}


const char* DeepCMultiply::node_help() const
{
    return
    "Multiply node for DeepC.\n\n"
    "Falk Hofmann 11/2021";
}


static Op* build(Node* node) { return new DeepCMultiply(node); }
const Op::Description DeepCMultiply::d("DeepCMultiply", 0, build);