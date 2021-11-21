/* AdjustBBox node for deep nodes.

Based on DeepCrop node from the Foundry.
Falk Hofmann, 11/2021

*/
#include "DDImage/DeepFilterOp.h"
#include "DDImage/DeepPixelOp.h"
#include "DDImage/Knobs.h"

static const char* CLASS = "DeepCKeyMix";

using namespace DD::Image;

static const char* const bbox_names[] = {"union", "B\tB side", "A\tA side", nullptr};

class DeepCKeyMix : public DeepPixelOp
{

  ChannelSet channels;
  Channel maskChannel;
  bool invertMask;
  float mix;
  // what to do with bbox:
  enum { UNION, BBOX, ABOX };
  int bbox_type;

public:

  DeepCKeyMix(Node* node) : DeepPixelOp(node) {

    inputs(3);
    channels = Mask_All;
    maskChannel = Chan_Alpha;
    invertMask = false;
    mix = 1;
    bbox_type = UNION;
  }

  const char* input_label(int n, char*) const
  {
    switch (n) {
      case 0: return "B";
      case 1: return "A";
      case 2: return "mask";
    }
  }

  const char* node_help() const override
  {
    return "KeyMix.\n"
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
    DeepFilterOp::_validate(for_real);
    DeepOp* A = dynamic_cast<DeepOp*>(Op::input1());
    DeepOp* B = dynamic_cast<DeepOp*>(Op::input0());

    DeepInfo infoA = A->deepInfo();
    DeepInfo infoB = B->deepInfo();

    infoB.merge(infoA);
    _deepInfo =infoB;
 
  }

  // Wrapper function to work around the "non-virtual thunk" issue on linux when symbol hiding is enabled.
  void getDeepRequests(Box box, const ChannelSet& channels, int count, std::vector<RequestData>& requests) override
  {
    DeepPixelOp::getDeepRequests(box, channels, count, requests);
  }


  bool doDeepEngine(Box box, const ChannelSet& channels, DeepOutputPlane& plane) override
  {    
    if (!input0())
      return true;

    DeepOp* B = input0();
    DeepOp* A = dynamic_cast<DeepOp*>(Op::input1());
    Iop* mask = dynamic_cast<Iop*>(Op::input(2));

    DeepPlane inPlane;

    ChannelSet needed = channels;
    needed += Mask_DeepFront;

    if (!B->deepEngine(box, needed, inPlane))
      return false;




    return true;
  }

  static const Description d;
};

static Op* build(Node* node) { return new DeepCKeyMix(node); }
const Op::Description DeepCKeyMix::d(::CLASS, "Image/DeepCKeyMix", build);
