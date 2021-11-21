/* Constant node for deep nodes with adjustable samples and depth range.

Falk Hofmann, 11/2021
*/

#include "DDImage/Iop.h"
#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"

static const char* CLASS = "DeepCConstant";
static const char* const enumAlphaTypes[]  = { "unform", "additive", "multiplicative", 0 };
using namespace DD::Image;

class DeepCConstant : public DeepFilterOp
{
    FormatPair formats;
    ChannelSet channels;
    float color[4];
    double _front;
    double _back;
    int _samples;
    float _overallDepth;
    float _sampleDistance;
    double _values[4];
    int _alphaType;
    float _modAlpha;

public:

  int minimum_inputs() const { return 0;}

  DeepCConstant(Node* node) : DeepFilterOp(node) {

    formats.format(nullptr);
    _front = 0;
    _back = 10;
    _samples = 1;
    _overallDepth = 1;
    _sampleDistance = 0.1f;
    color[0] = color[1] = color[2] = color[3] = 0.5;
    channels = Mask_RGBA;
    _alphaType = 2;
    _values[0] = _values[1] = _values[2] = _values[3] = 0.5;
    _modAlpha = 1.0;
  }
    void _validate(bool);
    bool doDeepEngine(DD::Image::Box bbox, const DD::Image::ChannelSet& requestedChannels, DeepOutputPlane& deepOutPlane);
    void knobs(Knob_Callback);
    int knob_changed(Knob* k);

    void calcValues();

    static const Iop::Description d;
    const char* Class() const { return d.name;}
    const char* node_help() const;
    virtual Op* op() { return this; }
};

  const char* DeepCConstant::node_help() const
      {return "A DeepConstant with defined deep range and amount of samples.\n\n"
              "Falk Hofmann 11/2021";}

  void DeepCConstant::knobs(Knob_Callback f)
  {
      Input_ChannelSet_knob(f, &channels, 4, "channels");
      AColor_knob(f, color, "color", "color");
      Format_knob(f, &formats, "format", "format");
      Obsolete_knob(f, "full_format", "knob format $value");
      Obsolete_knob(f, "proxy_format", nullptr);
      Divider(f, "");
      Enumeration_knob(f, &_alphaType, enumAlphaTypes, "alpha_mode", "split alpha mode");
      Tooltip(f, "Select the alpha mode.\n\n"
                 "multiplactive will match the image visually as compared to a 2d constant.\n\n"
                 "additive does a straight division by the number of samples and therefore will not match the 2d equivalent.\n\n"
                 "uniform applies the given color values straight to the samples, without considering and modifications due deep sample addition." );
      Double_knob(f, &_front, "front", "front");
      Tooltip(f, "Closest distance from camera.");
      Double_knob(f, &_back, "back", "back");
      Tooltip(f, "Farthest distance from camera.");
      Int_knob(f, &_samples, "samples", "samples");
      Tooltip(f, "Amount of deep samples per pixel.");
      SetRange(f, 2, 1000);
      SetFlags(f, Knob::NO_ANIMATION);

  }

  void DeepCConstant::calcValues(){

      float a = knob("color")->get_value(colourIndex(Chan_Alpha));
      int saveSample = (_samples > 0)? _samples: 1;
      _modAlpha = 1.0f- pow(1.0f - a, (1.0f/saveSample));


      for (int c = 0; c < 4; c++){

        float val = knob("color")->get_value(c);
        switch (_alphaType)
        {
        case 0:
          _values[c] = val;
          break;
        case 1:
          _values[c] = val/saveSample;
          break;
        case 2:
          _values[c] = (val/a) * _modAlpha;
        }
      }
  }


  int DeepCConstant::knob_changed(Knob* k){
    if (k->name() == "alpha_mode"){
      DeepCConstant::calcValues();
    }
  }

  void DeepCConstant::_validate(bool for_real)
  {
  ChannelSet new_channelset;
  new_channelset += Mask_Deep;
  new_channelset += Mask_Z;
  new_channelset += Mask_Alpha;
  new_channelset += channels;

  Box box(0, 0, formats.format()->width(), formats.format()->height()) ;
  _deepInfo = DeepInfo(formats, box, new_channelset);
  _overallDepth = _back - _front;
  _sampleDistance = _overallDepth/(_samples);
  DeepCConstant::calcValues();
  }

  bool DeepCConstant::doDeepEngine(DD::Image::Box box, const DD::Image::ChannelSet& channels, DeepOutputPlane& plane)
  {
  int saveSample = (_samples > 0) ? _samples: 1;

  DeepInPlaceOutputPlane outPlane(channels, box, DeepPixel::eZDescending);
  outPlane.reserveSamples(box.area());

  for (Box::iterator it = box.begin();
        it != box.end();
        it++) {

    outPlane.setSampleCount(it, saveSample);
    DeepOutputPixel out = outPlane.getPixel(it);
    for (int sampleNo = 0; sampleNo < saveSample; sampleNo++){

      foreach (z, channels) {
        float& output = out.getWritableOrderedSample(sampleNo, z);
        int cIndex = colourIndex(z);
        
        if (z == Chan_DeepFront){
          output = _front + (_sampleDistance * sampleNo);

        }
        else if (z == Chan_DeepBack){
          output = _front + (_sampleDistance * sampleNo) + _sampleDistance;

          }
          else{
          output = _values[cIndex];           
        }
      }
    }
  }
  mFnAssert(outPlane.isComplete());
  plane = outPlane;


  return true;
}

static Op* build(Node* node) { return new DeepCConstant(node); }
const Op::Description DeepCConstant::d("DeepCConstant", 0, build);
