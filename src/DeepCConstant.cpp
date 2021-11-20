/* Constant node for deep nodes with adjustable samples.

Falk Hofmann, 11/2021

*/

#include "DDImage/Iop.h"
#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"

static const char* CLASS = "DeepCConstant";

using namespace DD::Image;

class DeepCConstant : public DeepFilterOp
{
    FormatPair formats;
    Channel channel[4];
    float color[4];
    double _front;
    double _back;
    int _samples;
    int _firstFrame, lastFrame;
    float _overallDepth;
    float _sampleDistance;

public:

  int minimum_inputs() const { return 0;}

  DeepCConstant(Node* node) : DeepFilterOp(node) {

    formats.format(nullptr);
    _front = 0;
    _back = 10;
    _samples = 2;
    _overallDepth = 1;
    _sampleDistance = 0.1f;
    color[0] = color[1] = color[2] = color[3] = 0.5;
    _firstFrame = 1;
    lastFrame = 1;
    channel[0] = Chan_Red;
    channel[1] = Chan_Green;
    channel[2] = Chan_Blue;
    channel[3] = Chan_Alpha;
  }
    void _validate(bool);
    bool doDeepEngine(DD::Image::Box bbox, const DD::Image::ChannelSet& requestedChannels, DeepOutputPlane& deepOutPlane);
    void knobs(Knob_Callback);

    static const Iop::Description d;
    const char* Class() const { return d.name;}
    const char* node_help() const;
    virtual Op* op() { return this; }
};

    const char* DeepCConstant::node_help() const
        {return "Draw a constant with defined sample count.\n"
                "Falk Hofmann 11/2021";}

    void DeepCConstant::knobs(Knob_Callback f)
    {
        Channel_knob(f, channel, 4, "channels");
        AColor_knob(f, color, "color", "color");
        Format_knob(f, &formats, "format", "format");
        Obsolete_knob(f, "full_format", "knob format $value");
        Obsolete_knob(f, "proxy_format", nullptr);
        Divider(f, "");
        Double_knob(f, &_front, "front", "back");
        Double_knob(f, &_back, "back", "back");
        Int_knob(f, &_samples, "samples", "samples");
        SetRange(f, 2, 1000);/*  */
        SetFlags(f, Knob::NO_ANIMATION);

    }

    void DeepCConstant::_validate(bool for_real)
    {


    ChannelSet new_channelset;
    new_channelset += Mask_Deep;
    new_channelset += Mask_Z;

    for (int z = 0; z < 4; z++) {
      new_channelset += channel[z];
      }

    Box box(0, 0, formats.format()->width(), formats.format()->height()) ;
    _deepInfo = DeepInfo(formats, box, new_channelset);
    _overallDepth = _back - _front;
    _sampleDistance = _overallDepth/(_samples - 1);
    }


    bool DeepCConstant::doDeepEngine(DD::Image::Box box, const DD::Image::ChannelSet& channels, DeepOutputPlane& plane)
   {
     if (_samples < 2){
       _samples = 2;
     }

    DeepInPlaceOutputPlane outPlane(channels, box, DeepPixel::eZAscending);
    outPlane.reserveSamples(box.area());

    for (Box::iterator it = box.begin();
         it != box.end();
         it++) {

      outPlane.setSampleCount(it, _samples);
      DeepOutputPixel out = outPlane.getPixel(it);
      for (int sampleNo = 0; sampleNo < _samples; sampleNo++){

        foreach (z, channels) {
          float& output = out.getWritableOrderedSample(sampleNo, z);

          if (z == Chan_DeepFront || z == Chan_DeepBack){
            output = _front + (_sampleDistance * sampleNo);
           }else{
            int cIndex = colourIndex(z);
            output = color[cIndex];           
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
