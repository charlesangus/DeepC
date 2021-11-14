#include "DeepCWrapper.h"

using namespace DD::Image;

class DeepCGrade : public DeepCWrapper
{

    // grading
    float blackpoint[3];
    float whitepoint[3];
    float black[3];
    float white[3];
    float multiply[3];
    float add[3];
    float gamma[3];

    // precalculated grade coefficients
    float A[3];
    float B[3];
    float G[3];

    //
    bool _reverse;
    bool _blackClamp;
    bool _whiteClamp;

    public:

        DeepCGrade(Node* node) : DeepCWrapper(node)
        {
            for (int i=0; i<3; i++)
            {
                blackpoint[i] = 0.0f;
                whitepoint[i] = 1.0f;
                black[i] = 0.0f;
                white[i] = 1.0f;
                multiply[i] = 1.0f;
                add[i] = 0.0f;
                gamma[i] = 1.0f;
            }

            _reverse = false;
            _blackClamp = false;
            _whiteClamp = false;
        }

        void _validate(bool);

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

void DeepCGrade::_validate(bool for_real)
{
    DeepCWrapper::_validate(for_real);

    for (int i = 0; i < 3; i++) {
        // make safe the gamma values
        gamma[i] = clamp(gamma[i], 0.00001f, 65500.0f);

        // for calculating the grade - precompute the coefficients
        A[i] = multiply[i] * ((white[i] - black[i]) / (whitepoint[i] - blackpoint[i]));
        B[i] = add[i] + black[i] - A[i] * blackpoint[i];
        G[i] = 1.0f / gamma[i];
        if (_reverse)
        {
            // opposite linear ramp
            if (A[i])
            {
                A[i] = 1.0f / A[i];
            } else
            {
                A[i] = 1.0f;
            }
            B[i] = -B[i] * A[i];
            // inverse gamma
            G[i] = 1.0f/G[i];
        }
    }
}

/*
The guts. Do any processing on the channel value. The result will be masked
and mixed appropriately.
*/
void DeepCGrade::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData
    )
{
    int cIndex = colourIndex(z);
    if (_reverse)
    {
        // opposite gamma, precomputed
        if (G[cIndex] != 1.0f)
            outData = pow(inputVal, G[cIndex]);
        // then inverse linear ramp we have already precomputed
        outData = A[cIndex] * inputVal + B[cIndex];
        if (_blackClamp && outData < 0.0){outData = 0.0;}
        if (_whiteClamp && outData > 1.0){outData = 1.0;}
    } else
    {
        outData = A[cIndex] * inputVal + B[cIndex];
        if (G[cIndex] != 1.0f)
            outData = pow(outData, G[cIndex]);
        
        if (_blackClamp && outData < 0.0){outData = 0.0;}
        if (_whiteClamp && outData > 1.0){outData = 1.0;}
    }
}


void DeepCGrade::custom_knobs(Knob_Callback f)
{
    Color_knob(f, blackpoint, IRange(-1, 1), "blackpoint", "blackpoint");
    Tooltip(f, "This color is turned into black");
    Color_knob(f, whitepoint, IRange(0, 4), "whitepoint", "whitepoint");
    Tooltip(f, "This color is turned into white");
    Color_knob(f, black, IRange(-1, 1), "black", "lift");
    Tooltip(f, "Black is turned into this color");
    Color_knob(f, white, IRange(0, 4), "white", "gain");
    Tooltip(f, "White is turned into this color");
    Color_knob(f, multiply, IRange(0, 4), "multiply", "multiply");
    Tooltip(f, "Constant to multiply result by");
    Color_knob(f, add, IRange(-1, 1), "add", "offset");
    Tooltip(f, "Constant to add to result (raises both black & white, unlike lift)");
    Color_knob(f, gamma, IRange(0.2, 5), "gamma", "gamma");
    Tooltip(f, "Gamma correction applied to final result");
    Bool_knob(f, &_reverse, "reverse");
    SetFlags(f, Knob::STARTLINE);
    Bool_knob(f, &_blackClamp, "black_clamp");
    Bool_knob(f, &_whiteClamp, "white_clamp");
}


const char* DeepCGrade::node_help() const
{
    return
    "Grade node for DeepC.";
}


static Op* build(Node* node) { return new DeepCGrade(node); }
const Op::Description DeepCGrade::d("DeepCGrade", 0, build);
