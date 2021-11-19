/* CopyBBox node for deep nodes.

Based on DeepCrop node from the Foundry.
Falk Hofmann, 11/2021

*/

#include "DDImage/DeepFilterOp.h"

static const char* CLASS = "DeepCCopyBbox";

using namespace DD::Image;

class DeepCCopyBbox : public DeepFilterOp
{
  float _bbox[4];

public:
  
  int minimum_inputs() const { return 2; }
  int maximum_inputs() const { return 2; }

  DeepCCopyBbox(Node* node) : DeepFilterOp(node) {

    _bbox[0] = 0;
    _bbox[1] = 0;
    _bbox[2] = input_format().width();
    _bbox[3] = input_format().height();
  }

  const char* node_help() const override
  {
    return "Copy the bounding box from one input into the stream.\n"
           "Falk Hofmann 11/2021";
  }

  const char* input_label(int n, char*) const
  {
    switch (n) {
      case 0: return "";
      case 1: return "bbox";
    }
  }

  const char* Class() const override {
    return CLASS;
  }

  Op* op() override
  {
    return this;
  }


  void _validate(bool for_real) override
    {
    DeepFilterOp::_validate(for_real);
    DeepOp* second = dynamic_cast<DeepOp*>(Op::input1());
    if (input0() && second) {
      second->validate(for_real);
      DeepInfo d = second->deepInfo();
      _bbox[0] = d.x();
      _bbox[1] = d.y();
      _bbox[2] = d.r();
      _bbox[3] = d.t();
      _deepInfo.box().set(floor(_bbox[0] - 1), floor(_bbox[1] - 1), ceil(_bbox[2] + 1), ceil(_bbox[3] + 1));
    }else if (input0() && !input1()){
      _bbox[0] = _deepInfo.x();
      _bbox[1] = _deepInfo.y();
      _bbox[2] = _deepInfo.r();
      _bbox[3] = _deepInfo.t();
    }
  }

  bool doDeepEngine(DD::Image::Box box, const ChannelSet& channels, DeepOutputPlane& plane) override
  {
    if (!input0())
      return true;

    DeepOp* in = input0();
    DeepPlane inPlane;

    ChannelSet needed = channels;
    needed += Mask_DeepFront;

    if (!in->deepEngine(box, needed, inPlane))
      return false;

    DeepInPlaceOutputPlane outPlane(channels, box);
    outPlane.reserveSamples(inPlane.getTotalSampleCount());

    std::vector<int> validSamples;

    for (DD::Image::Box::iterator it = box.begin(), itEnd = box.end(); it != itEnd; ++it) {

      const int x = it.x;
      const int y = it.y;

      bool inXY = (x >= _bbox[0] && x < _bbox[2] && y >= _bbox[1] && y < _bbox[3]);

      if (!inXY) {
        outPlane.setSampleCount(y, x, 0);
        continue;
      }

      DeepPixel inPixel = inPlane.getPixel(it);
      size_t inPixelSamples = inPixel.getSampleCount();

      validSamples.clear();
      validSamples.reserve(inPixelSamples);

      for (size_t iSample = 0; iSample < inPixelSamples; ++iSample) {
        validSamples.push_back(iSample);
      }

      outPlane.setSampleCount(it, validSamples.size());
      DeepOutputPixel outPixel = outPlane.getPixel(it);

      size_t outSample = 0;
      for (size_t inSample : validSamples)
      {
        const float* inData = inPixel.getUnorderedSample(inSample);
        float* outData = outPixel.getWritableUnorderedSample(outSample);
        for ( int iChannel = 0, nChannels = channels.size();
              iChannel < nChannels;
              ++iChannel, ++outData, ++inData
              ) {
          *outData = *inData;
        }
        ++outSample;
      }
    }

    outPlane.reviseSamples();
    mFnAssert(outPlane.isComplete());
    plane = outPlane;

    return true;
  }

  static const Description d;
};

static Op* build(Node* node) { return new DeepCCopyBbox(node); }
const Op::Description DeepCCopyBbox::d(::CLASS, "Image/DeepCCopyBbox", build);
