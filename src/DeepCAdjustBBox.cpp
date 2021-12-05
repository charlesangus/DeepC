/* AdjustBBox node for deep nodes.

Based on DeepCrop node from the Foundry.
Falk Hofmann, 11/2021

*/
#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"

static const char* CLASS = "DeepCAdjustBBox";

using namespace DD::Image;

class DeepCAdjustBBox : public DeepFilterOp
{

  double numpixels[2];
  float _bbox[4];

public:
  DeepCAdjustBBox(Node* node) : DeepFilterOp(node) {
    _bbox[0] = _bbox[2] = _bbox[1] = _bbox[3] = 0;
  }

  const char* node_help() const override
  {
    return "Increase or reduce the size of the bouding box.\n\n"
           "Falk Hofmann 11/2021";
  }

  const char* Class() const override {
    return CLASS;
  }

  Op* op() override
  {
    return this;
  }

  void knobs(Knob_Callback f) override
  {
    WH_knob(f, numpixels, "numpixels", "Add Pixels");
    SetRange(f, -1024, 1024);
    Tooltip(f, "Add or remove pixels from the incoming bounding box.");
  }

  void _validate(bool for_real) override
  {
    DeepFilterOp::_validate(for_real);
    _bbox[0] = _deepInfo.x() - numpixels[0];
    _bbox[1] = _deepInfo.y() - numpixels[1];
    _bbox[2] = _deepInfo.r() + numpixels[0];
    _bbox[3] = _deepInfo.t() + numpixels[1];
    _deepInfo.box().set(floor(_bbox[0] - 1), floor(_bbox[1] - 1), ceil(_bbox[2] + 1), ceil(_bbox[3] + 1));
  
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

    //samples per pixel after croping
    std::vector<int> validSamples;

    for (DD::Image::Box::iterator it = box.begin(), itEnd = box.end(); it != itEnd; ++it) {

      const int x = it.x;
      const int y = it.y;

      bool inXY = (x >= _bbox[0] && x < _bbox[2] && y >= _bbox[1] && y < _bbox[3]);

      if (!inXY) {
        // pixel out of crop bounds
        outPlane.setSampleCount(y, x, 0);
        continue;
      }

      DeepPixel inPixel = inPlane.getPixel(it);
      size_t inPixelSamples = inPixel.getSampleCount();

      validSamples.clear();
      validSamples.reserve(inPixelSamples);

      // find valid samples
      for (size_t iSample = 0; iSample < inPixelSamples; ++iSample) {

        validSamples.push_back(iSample);
      }

      outPlane.setSampleCount(it, validSamples.size());

      DeepOutputPixel outPixel = outPlane.getPixel(it);

      // copy valid samples to DeepOutputPlane
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

static Op* build(Node* node) { return new DeepCAdjustBBox(node); }
const Op::Description DeepCAdjustBBox::d(::CLASS, "Image/DeepCAdjustBBox", build);
