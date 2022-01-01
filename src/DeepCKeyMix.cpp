// Copyright (c) 2011 The Foundry Visionmongers Ltd.  All Rights Reserved.

#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/Row.h"

static const char *CLASS = "DeepCKeyMix";
static const char *HELP = "DeepCKeyMix";

using namespace DD::Image;

static const char *const bbox_names[] = {"union", "B\tB side", "A\tA side", nullptr};

class DeepCKeyMix : public DeepFilterOp
{

    ChannelSet channels;
    Channel maskChannel;
    bool invertMask;
    float mix;
    // what to do with bbox:
    enum
    {
        UNION,
        BBOX,
        ABOX
    };
    int bbox_type;
    // Iop *_maskOp;

protected:
    Iop *_maskOp;
    DeepOp *_aOp;
    DeepOp *_bOp;

public:
    DeepCKeyMix(Node *node) : DeepFilterOp(node)
    {
        inputs(3);

        channels = Mask_All;
        maskChannel = Chan_Alpha;
        invertMask = false;
        mix = 1;
        bbox_type = UNION;
    }

    const char *node_help() const override { return HELP; }
    const char *Class() const override { return CLASS; }
    Op *op() override { return this; }

    bool test_input(int input, Op *op) const
    {
        switch (input)
        {
        case 0:
        case 1:
            return DeepFilterOp::test_input(input, op);
        case 2:
            return dynamic_cast<Iop *>(op) != 0;
        default:
            return DeepFilterOp::test_input(input, op);
        }
    }

    const char *input_label(int n, char *) const
    {
        switch (n)
        {
        case 0:
            return "B";
        case 1:
            return "A";
        case 2:
            return "mask";
        }
    }

    void knobs(Knob_Callback f) override
    {
        Input_ChannelMask_knob(f, &channels, 1, "channels");
        Tooltip(f, "Channels to copy from A. Other channels are copied unchanged from B");
        Input_Channel_knob(f, &maskChannel, 1, 2, "maskChannel", "mask channel");
        Tooltip(f, "Channel to use from mask input");
        Bool_knob(f, &invertMask, "invertMask", "invert");
        Tooltip(f, "Flip meaning of the the mask channel");
        Float_knob(f, &mix, "mix");
        Tooltip(f, "Dissolve between B-only at 0 and the full keymix at 1");
        Enumeration_knob(f, &bbox_type, bbox_names, "bbox", "Set BBox to");
        Tooltip(f, "Clip one input to match the other if wanted");
    }

    void _validate(bool for_real) override
    {
        _aOp = dynamic_cast<DeepOp *>(Op::input(1));
        _bOp = input0();
        _maskOp = dynamic_cast<Iop *>(Op::input(2));
        if (_bOp != nullptr)
        {
            if (_aOp != nullptr && _maskOp != nullptr)
            {
                _aOp->validate(for_real);
                _bOp->validate(for_real);
                _maskOp->validate(for_real);

                DeepInfo infoA = _aOp->deepInfo();
                DeepInfo infoB = _bOp->deepInfo();
                infoB.merge(infoA);
                _deepInfo = infoB;
            }
            else
            {
                _deepInfo = _bOp->deepInfo();
            }
        }
        else
        {
            DeepFilterOp::_validate(for_real);
        }
    }

    void getDeepRequests(Box box, const ChannelSet &channels, int count, std::vector<RequestData> &requests)
    {
        if (!input0())
            return;

        ChannelSet requestChannels = channels;
        requestChannels += Mask_Deep;
        requests.push_back(RequestData(input0(), box, requestChannels, count));

        if (_aOp != nullptr)
        {
            requests.push_back(RequestData(_aOp, box, requestChannels, count));
        }
        if (_maskOp != nullptr)
        {
            _maskOp->request(box, maskChannel, count);
        }
    }

    bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane) override
    {
        // DeepOp *bOp = input0();
        // if (!bOp)
        //     return true;

        // DeepOp *aOp = dynamic_cast<DeepOp *>(Op::input1());
        // // Iop *maskOp = dynamic_cast<Iop *>(Op::input(2));
        // if (!aOp)
        //     return false;
        // // if (_maskOp != NULL)
        // //     return false;

        // DeepPlane aPlane;
        // DeepPlane bPlane;

        // // if (!bOp->deepEngine(box, needed, bPlane))
        // //     return false;

        // // if (!aOp->deepEngine(box, needed, aPlane))
        // //     return false;

        // DeepInPlaceOutputPlane outPlane(channels, box);
        // outPlane.reserveSamples(bPlane.getTotalSampleCount());

        // std::vector<int> validSamples;

        // // float maskVal;
        // // int currentYRow;
        // // Row maskRow(box.x(), box.r());

        // // _maskOp->get(box.y(), box.x(), box.r(), maskChannel, maskRow);
        // // currentYRow = box.y();

        // for (Box::iterator it = box.begin(), itEnd = box.end(); it != itEnd; ++it)
        // {
        //     // we have not already gotten this row, get it now
        //     // _maskOp->get(it.y, box.x(), box.r(), maskChannel, maskRow);
        //     // maskVal = maskRow[maskChannel][it.x];
        //     // maskVal = clamp(maskVal);
        //     // currentYRow = it.y;

        //     DeepPixel bPixel = bPlane.getPixel(it);
        //     DeepPixel aPixel = aPlane.getPixel(it);

        //     size_t inPixelSamples = bPixel.getSampleCount();

        //     validSamples.clear();
        //     validSamples.reserve(inPixelSamples);

        //     // find valid samples
        //     for (size_t iSample = 0; iSample < inPixelSamples; ++iSample)
        //     {
        //         validSamples.push_back(iSample);
        //     }

        //     outPlane.setSampleCount(it, validSamples.size());

        //     DeepOutputPixel outPixel = outPlane.getPixel(it);

        //     // copy valid samples to DeepOutputPlane
        //     size_t outSample = 0;
        //     for (size_t inSample : validSamples)
        //     {
        //         const float *inData = bPixel.getUnorderedSample(inSample);
        //         float *outData = outPixel.getWritableUnorderedSample(outSample);
        //         for (int iChannel = 0, nChannels = channels.size();
        //              iChannel < nChannels;
        //              ++iChannel, ++outData, ++inData)
        //         {
        //             *outData = *inData;
        //         }
        //         ++outSample;
        //     }
        // }

        // outPlane.reviseSamples();
        // mFnAssert(outPlane.isComplete());
        // plane = outPlane;

        return true;
    }

    static const Description d;
};

static Op *build(Node *node) { return new DeepCKeyMix(node); }
const Op::Description DeepCKeyMix::d(::CLASS, 0, build);
