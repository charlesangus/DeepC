#include "DeepCWrapper.h"

#include <string>
using namespace DD::Image;
using namespace std;

static const char* auxChannelName = "position_data";
enum {REPLACE, UNION, MASK, STENCIL, OUT, MIN_OP, MAX_OP};
static const char* const operationNames[] = {
    "replace",
    "union",
    "mask",
    "stencil",
    "out",
    "min",
    "max",
    0
    };

class DeepCMWrapper : public DeepCWrapper
{
    protected:
        // can override in child classes to change the name of the aux channel knob
        const char* _auxChannelKnobName;

        ChannelSet _auxiliaryChannelSet;
        bool _unpremultPosition;

        int _operation;

    public:

        DeepCMWrapper(Node* node) : DeepCWrapper(node)
            , _auxChannelKnobName("input_data")
            , _auxiliaryChannelSet(Chan_Black)
            // , _premultOutput(true)
            , _unpremultPosition(true)
            , _operation(0)
        {

        }
        virtual void findNeededDeepChannels(
            ChannelSet& neededDeepChannels
            );
        virtual void _validate(bool);

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

        virtual void top_knobs(Knob_Callback f);
        virtual void custom_knobs(Knob_Callback f);
        // virtual void knobs(Knob_Callback f);

        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};
