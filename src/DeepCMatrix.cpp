#include "DeepCWrapper.h"
#include "DDImage/RGB.h"
#include <DDImage/Convolve.h>

using namespace DD::Image;

class DeepCMatrix : public DeepCWrapper
{

    ConvolveArray _arrayKnob;
    Matrix3 array_mtx3;
    Matrix3 array_mtx3_inverse;
    ChannelSet _brothers;
    bool _invert;

public:
    DeepCMatrix(Node *node) : DeepCWrapper(node),
                              _arrayKnob()
    {
        array_mtx3.makeIdentity();
        _invert = false;
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


void DeepCMatrix::findNeededDeepChannels(ChannelSet& neededDeepChannels)
{
    DeepCWrapper::findNeededDeepChannels(neededDeepChannels);
    Channel firstChan;
    firstChan = _processChannelSet.first();
    _brothers = Chan_Black;
    _brothers.addBrothers(firstChan, 3);
    neededDeepChannels += _brothers;
}

void DeepCMatrix::_validate(bool for_real){

    DeepCWrapper::_validate(for_real);
    array_mtx3.set(_arrayKnob.array[0], _arrayKnob.array[1], _arrayKnob.array[2], 
                   _arrayKnob.array[3], _arrayKnob.array[4], _arrayKnob.array[5], 
                   _arrayKnob.array[6], _arrayKnob.array[7], _arrayKnob.array[8]);
    array_mtx3_inverse = array_mtx3.inverse();

}
void DeepCMatrix::wrappedPerSample(
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

    if (!_invert){
        sampleColor = array_mtx3.transform(sampleColor);
    }else{
        sampleColor = array_mtx3_inverse.transform(sampleColor);
    }
}

void DeepCMatrix::wrappedPerChannel(
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


void DeepCMatrix::custom_knobs(Knob_Callback f)
{
    Array_knob(f, &_arrayKnob, 3, 3, "matrix");
    Tooltip(f, "Output red is the first row multiplied by the input rgb.\n"
               "Output green is the second row multiplied by the input rgb.\n"
               "Output blue is the third row multiplied by the input rgb.\n");
    Bool_knob(f, &_invert, "invert", "invert");
    Tooltip(f, "Use the inverse of the matrix.");
}

const char* DeepCMatrix::node_help() const
{
    return "A Deep ColorMatrix node.\n\n"
           "Falk Hofmann 12/2021";
}


static Op* build(Node* node) { return new DeepCMatrix(node); }
const Op::Description DeepCMatrix::d("DeepCMatrix", 0, build);
