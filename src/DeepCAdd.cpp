#include "DeepCWrapper.h"

using namespace DD::Image;

class DeepCAdd : public DeepCWrapper
{
    float value[3];

    public:

        DeepCAdd(Node* node) : DeepCWrapper(node)
        {
            for (int i=0; i<3; i++)
            {
                value[i] = 0.0f;
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
void DeepCAdd::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData
    )
{
    int cIndex = colourIndex(z);
        outData = inputVal + value[cIndex];

}


void DeepCAdd::custom_knobs(Knob_Callback f)
{
    Color_knob(f, value, IRange(0, 5), "value", "value");

}


const char* DeepCAdd::node_help() const
{
    return
    "Add node for DeepC.";
}


static Op* build(Node* node) { return new DeepCAdd(node); }
const Op::Description DeepCAdd::d("DeepCAdd", 0, build);