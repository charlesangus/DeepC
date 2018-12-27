#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/RGB.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"
#include "DDImage/Row.h"
#include "DDImage/Black.h"

#include <stdio.h>

using namespace DD::Image;

class DeepCWrapper : public DeepFilterOp
{
    // values knobs write into go here
    ChannelSet _processChannelSet;
    bool _unpremult;

    // masking
    Channel _deepMaskChannel;
    bool _doDeepMask;
    bool _invertDeepMask;
    bool _unpremultDeepMask;

    Channel _sideMaskChannel;
    Channel _rememberedMaskChannel;
    Iop* _maskOp;
    bool _doSideMask;
    bool _invertSideMask;

    float _mix;

    float _gain;

    ChannelSet _allNeededDeepChannels;

    public:

        DeepCWrapper(Node* node) : DeepFilterOp(node),
            _processChannelSet(Mask_RGB),
            _unpremult(true),
            _deepMaskChannel(Chan_Black),
            _doDeepMask(false),
            _invertDeepMask(false),
            _unpremultDeepMask(true),
            _sideMaskChannel(Chan_Black),
            _rememberedMaskChannel(Chan_Black),
            _doSideMask(false),
            _invertSideMask(false),
            _mix(1.0f),
            _allNeededDeepChannels(Chan_Black)
        {
            _gain = 1.0f;
        }

        void findNeededDeepChannels(ChannelSet& neededDeepChannels);
        void _validate(bool);
        void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests);

        virtual void wrappedPerSample(Box::iterator it, DeepPixel deepInPixel, size_t sampleNo, float &perSampleData);
        virtual void wrappedPerChannel(const float inputVal, float perSampleData, Channel z, float& outData);

        bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane);
        void top_knobs(Knob_Callback f);
        virtual void custom_knobs(Knob_Callback f);
        void bottom_knobs(Knob_Callback f);
        void knobs(Knob_Callback);
        int knob_changed(DD::Image::Knob* k);


        bool test_input(int n, Op *op)  const;
        Op* default_input(int input) const;
        const char* input_label(int input, char* buffer) const;
        int minimum_inputs() const { return 2; }
        int maximum_inputs() const { return 2; }
        int optional_input() const { return 1; }
        static const Iop::Description d;
        const char* Class() const { return d.name; }
        Op* op() { return this; }
        const char* node_help() const;
};
