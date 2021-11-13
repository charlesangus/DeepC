#include "DeepCWrapper.h"
#include "DDImage/ColorLookup.h"
#include "DDImage/LookupCurves.h"

using namespace DD::Image;

static const CurveDescription defaults[] = {
  { "master", "y C 0 1" },
  { "red",    "y C 0 1" },
  { "green",  "y C 0 1" },
  { "blue",   "y C 0 1" },
  { nullptr }
};

class DeepCColorLookup : public DeepCWrapper
{
    LookupCurves lut;
    float range;
    float range_knob;
    float source_value[3];
    float target_value[3];

    public:

        DeepCColorLookup(Node* node) : DeepCWrapper(node), lut(defaults)
        {


        }
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

        float lookup(int z, float value){
            value = float(lut.getValue(0, value));
            value = float(lut.getValue(z + 1, value));
            return value;
        };
};


/*
The guts. Do any processing on the channel value. The result will be masked
and mixed appropriately.
*/
void DeepCColorLookup::wrappedPerChannel(
    const float inputVal,
    float perSampleData,
    Channel z,
    float& outData
    )
{
    int cIndex = colourIndex(z);
    outData = lookup(colourIndex(z), inputVal);
}

const char* setRgbScript =
  "source = nuke.thisNode().knob('source')\n"
  "target = nuke.thisNode().knob('target')\n"
  "lut = nuke.thisNode().knob('lut')\n"
  "lut.setValueAt(target.getValue(0), source.getValue(0), 1)\n"
  "lut.setValueAt(target.getValue(1), source.getValue(1), 2)\n"
  "lut.setValueAt(target.getValue(2), source.getValue(2), 3)\n";


void DeepCColorLookup::custom_knobs(Knob_Callback f)
{
    LookupCurves_knob(f, &lut, "lut");
    Newline(f);
    AColor_knob(f, source_value, IRange(0, 4), "source");
    SetFlags(f, Knob::NO_ANIMATION | Knob::NO_RERENDER | Knob::DO_NOT_WRITE);
    Tooltip(f, "Pick a source color for adding points.");
    AColor_knob(f, target_value, IRange(0, 4), "target");
    SetFlags(f, Knob::NO_ANIMATION | Knob::NO_RERENDER | Knob::DO_NOT_WRITE);
    Tooltip(f, "Pick a destination color for adding points.");
    Newline(f);
    PyScript_knob(f, setRgbScript, "setRGB", "Set RGB");
    Tooltip(f, "Add points on the r, g, b curves mapping source to target.");


}

const char* DeepCColorLookup::node_help() const
{
    return
    "ColorLookup node for DeepC.";
}


static Op* build(Node* node) { return new DeepCColorLookup(node); }
const Op::Description DeepCColorLookup::d("DeepCColorLookup", 0, build);