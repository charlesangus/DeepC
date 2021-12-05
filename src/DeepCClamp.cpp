#include "DeepCWrapper.h"

using namespace DD::Image;

class DeepCClamp : public DeepCWrapper
{
    bool _minClamp;
    bool _maxClamp;
    bool _minClampTo;
    bool _maxClampTo;

    float minValue[3];
    float maxValue[3];
    float minClampToValue[3];
    float maxClampToValue[3];

    public:

        DeepCClamp(Node* node) : DeepCWrapper(node)
        {
            for (int i=0; i<3; i++)
            {
                minValue[i] = 0.0f;
                minClampToValue[i] = 0.0f;
            }

            for (int i=0; i<3; i++)
            {
                maxValue[i] = 1.0f;
                maxClampToValue[i] = 1.0f;
            }

            _minClamp = _maxClamp = true;
            _minClampTo = _maxClampTo = false;
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
void DeepCClamp::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData
    )
{
    int cIndex = colourIndex(z);

    float v = inputVal;

    if (_minClamp && inputVal < minValue[cIndex]){
        if (_minClampTo){
             v = minClampToValue[cIndex];
         }else{
             v = minValue[cIndex];
         }
    }
    if (_maxClamp && inputVal > maxValue[cIndex]){
        if (_maxClampTo){
            v = maxClampToValue[cIndex];
        }else{
            v = maxValue[cIndex];
        }
    }
    outData = v;   
}


void DeepCClamp::custom_knobs(Knob_Callback f)
{
    
    Color_knob(f, minValue, IRange(0, 1), "minimum", "minimum");
    Bool_knob(f, &_minClamp, "minimum_enable", "enable");
    Color_knob(f, maxValue, IRange(0, 1), "maximum", "maximum");
    Bool_knob(f, &_maxClamp, "maximum_enable", "enable");
    Color_knob(f, minClampToValue, IRange(0, 1), "MinClampTo", "MinClampTo");
    Bool_knob(f, &_minClampTo, "MinClampTo_enable", "enable");
    Color_knob(f, maxClampToValue, IRange(0, 1), "MaxClampTo", "MaxClampTo");
    Bool_knob(f, &_maxClampTo, "MaxClampTo_enable", "enable");
}


const char* DeepCClamp::node_help() const
{
    return
    "Clamp node for DeepC.\n\n"
    "Falk Hofmann 11/2021";
}


static Op* build(Node* node) { return new DeepCClamp(node); }
const Op::Description DeepCClamp::d("DeepCClamp", 0, build);