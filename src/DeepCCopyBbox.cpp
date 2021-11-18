// Copyright (c) 2011 The Foundry Visionmongers Ltd.  All Rights Reserved.

#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"
#include <string>
static const char* CLASS = "DeepCCopyBbox";

using namespace std;
using namespace DD::Image;

class DeepCCopyBbox : public DeepFilterOp
{

  float _bbox[4];

public:

  int minimum_inputs() const { return 2; }
  int maximum_inputs() const { return 2; }

  DeepCCopyBbox(Node* node) : DeepFilterOp(node) {

    _bbox[0] = _bbox[2] = 100,
    _bbox[1] = _bbox[3] = 200;

  }

  const char* input_label(int n, char*) const
  {
    switch (n) {
      case 0: return "";
      case 1: return "bbox";
    }
  }
  const char* node_help() const override{return "Crop deep data in front of or behind certain planes, or inside or outside of a box.";}
  const char* Class() const override {return CLASS;}

  Op* op() override
  {
    return this;
  }

  void knobs(Knob_Callback f) override
  {


  }

  void _validate(bool for_real) override
  {
    if (input0() && !input1()) {
      input0()->validate(for_real);
      _deepInfo = input0()->deepInfo();
    }else if (input0() && input1()) {
      input0()->validate(for_real);
      input1()->validate(for_real);

      DeepOp* in = dynamic_cast<DeepOp*>(Op::input1());
      _deepInfo = in->deepInfo();
      _bbox[0] = _deepInfo.x();
      _bbox[1] = _deepInfo.r();
      _bbox[2] = _deepInfo.t();
      _bbox[3] = _deepInfo.y();

      _deepInfo = input0()->deepInfo();
      // _deepInfo.box().set(floor(_bbox[0]), floor(_bbox[1] ), ceil(_bbox[2]), ceil(_bbox[3] + 1));
      _deepInfo.box().set(100, 100, 1500, 1500);

    }else {
          _deepInfo = DeepInfo();
    }

  }

  bool doDeepEngine(DD::Image::Box box, const ChannelSet& channels, DeepOutputPlane& plane) override
  {
    if (!input0() && !input1())
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

      bool inside = (x >= _bbox[0] && x < _bbox[2] && y >= _bbox[1] && y < _bbox[3]);

      if (inside) {
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
