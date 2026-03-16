/**
 * DeepCTest — temporary diagnostic node for Bug 3 investigation.
 *
 * Tests whether ChannelSet_knob set_text() updates the backing C++ member
 * synchronously before knob_changed() fires.
 *
 * How to use:
 *   1. Load DeepCTest.so into Nuke and create a DeepCTest node.
 *   2. Open the node panel. Note the initial "result" value.
 *   3. Change the "layer" picker from one layer to another.
 *   4. Immediately read "result":
 *        - If it shows the NEW layer's channels → set_text() is synchronous.
 *        - If it shows the OLD layer's channels → one-step lag confirmed.
 *   5. "flag" toggles each time knob_changed fires for "layer", confirming
 *      the callback is reached at all.
 *
 * Remove this node and its CMakeLists entry after diagnosis is complete.
 */

#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"
#include <sstream>
#include <string>

using namespace DD::Image;

class DeepCTest : public DeepFilterOp
{
    ChannelSet  _layerChannelSet;   // backing member for the ChannelSet knob under test
    std::string _diagnosisResult;   // written in knob_changed to show current _layerChannelSet
    bool        _toggleFlag;        // flips each time knob_changed fires for "layer"

public:
    DeepCTest(Node* node) : DeepFilterOp(node)
    {
        _layerChannelSet = Mask_RGBA;
        _diagnosisResult = "not yet changed";
        _toggleFlag      = false;
    }

    void _validate(bool for_real) override
    {
        DeepFilterOp::_validate(for_real);
    }

    void getDeepRequests(Box bbox, const ChannelSet& requestedChannels,
                         int count, std::vector<RequestData>& requests) override
    {
        // Pass-through: request the same channels from the single input.
        requests.push_back(RequestData(input0(), bbox, requestedChannels, count));
    }

    bool doDeepEngine(Box bbox, const ChannelSet& requestedChannels,
                      DeepOutputPlane& deepOutPlane) override
    {
        // Pass-through: copy input deep samples to output unchanged.
        DeepPlane deepInPlane;
        if (!input0()->deepEngine(bbox, requestedChannels, deepInPlane))
            return false;

        DeepInPlaceOutputPlane inPlaceOutPlane(requestedChannels, bbox);
        inPlaceOutPlane.reserveSamples(deepInPlane.getTotalSampleCount());

        for (Box::iterator it = bbox.begin(); it != bbox.end(); ++it)
        {
            DeepPixel inPixel = deepInPlane.getPixel(it);
            const size_t sampleCount = inPixel.getSampleCount();
            inPlaceOutPlane.setSampleCount(it, sampleCount);
            DeepOutputPixel outPixel = inPlaceOutPlane.getPixel(it);
            for (size_t sampleNo = 0; sampleNo < sampleCount; ++sampleNo)
            {
                foreach(z, requestedChannels)
                    outPixel.getWritableUnorderedSample(sampleNo, z) =
                        inPixel.getUnorderedSample(sampleNo, z);
            }
        }

        mFnAssert(inPlaceOutPlane.isComplete());
        deepOutPlane = inPlaceOutPlane;
        return true;
    }

    void knobs(Knob_Callback f) override
    {
        Input_ChannelSet_knob(f, &_layerChannelSet, 0, "layer", "layer");

        // "result" shows _layerChannelSet contents at the moment knob_changed fires.
        // Read-only in practice — we write to it via set_text() in knob_changed.
        String_knob(f, &_diagnosisResult, "result", "backing var at knob_changed time");

        // Flips each time "layer" triggers knob_changed — confirms the callback fires.
        Bool_knob(f, &_toggleFlag, "flag", "flag (flips each change)");
    }

    int knob_changed(Knob* changedKnob) override
    {
        if (changedKnob->is("layer"))
        {
            // Flip toggle to confirm this callback fired.
            _toggleFlag = !_toggleFlag;

            // Serialize _layerChannelSet exactly as it sits in the C++ member right now.
            // If set_text() is synchronous, this shows the NEW layer's channels.
            // If asynchronous (one-step lag), this shows the OLD layer's channels.
            std::ostringstream resultStream;
            resultStream << "flag=" << (_toggleFlag ? "T" : "F") << "  channels=";
            bool firstChannel = true;
            Channel ch;
            foreach(ch, _layerChannelSet)
            {
                if (!firstChannel) resultStream << ", ";
                resultStream << getName(ch);
                firstChannel = false;
            }
            if (firstChannel)
                resultStream << "(empty / Chan_Black)";

            _diagnosisResult = resultStream.str();

            // Push the result into the string knob so it is visible in the panel.
            if (Knob* resultKnob = knob("result"))
                resultKnob->set_text(_diagnosisResult.c_str());

            return 1;
        }
        return DeepFilterOp::knob_changed(changedKnob);
    }

    static const Iop::Description d;
    const char* Class() const override { return d.name; }
    Op* op() override { return this; }
    const char* node_help() const override
    {
        return
            "DIAGNOSTIC NODE — remove after Bug 3 investigation.\n\n"
            "Tests whether ChannelSet_knob set_text() updates the C++ backing "
            "member synchronously before knob_changed() fires.\n\n"
            "1. Change the 'layer' picker.\n"
            "2. Read 'result':\n"
            "     NEW channels shown → synchronous (fix is elsewhere).\n"
            "     OLD channels shown → one-step lag confirmed.\n"
            "3. 'flag' confirms the callback fired at all.";
    }
};

static Op* build(Node* node) { return new DeepCTest(node); }
const Op::Description DeepCTest::d("DeepCTest", 0, build);
