#include "DDImage/DeepFilterOp.h"
#include "DDImage/DeepSample.h"
#include "DDImage/Knobs.h"
#include "DDImage/RGB.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"
#include "DDImage/Matrix4.h"
#include "DDImage/Row.h"
#include "DDImage/Black.h"

#include <stdio.h>

using namespace DD::Image;


//Blink API includes
// #include "DDImage/Blink.h"
#include "Blink/Blink.h"


static const char* const shapeNames[] = { "sphere", "cube", 0 };
static const char* const falloffTypeNames[] = { "smooth", "linear", 0 };
enum { SMOOTH, LINEAR };

static const char* const GainKernel = "\
kernel GainKernel : ImageComputationKernel<eComponentWise>\n\
{\n\
  Image<eReadWrite> dst;\n\
\n\
  param:\n\
    float multiply;\n\
\n\
  void define() {\n\
    defineParam(multiply, \"Gain\", 1.0f);\n\
  }\n\
\n\
  void process() {\n\
    dst() *= multiply;\n\
   }\n\
};\n\
";

class DeepCBlink : public DeepFilterOp
{
    // values knobs write into go here
    ChannelSet _outputChannelSet;
    ChannelSet _auxiliaryChannelSet;
    bool _premultOutput;
    bool _unpremultPosition;

    float _positionPick[2];
    Matrix4 _axisKnob;

    int _shape;
    int _falloffType;
    float _falloff;
    float _falloffGamma;

    // masking
    Channel _deepMaskChannel;
    ChannelSet _deepMaskChannelSet;
    bool _doDeepMask;
    bool _invertDeepMask;
    bool _unpremultDeepMask;

    Channel _sideMaskChannel;
    Channel _rememberedMaskChannel;
    ChannelSet _sideMaskChannelSet;
    Iop* _maskOp;
    bool _doSideMask;
    bool _invertSideMask;
    float _mix;

    ChannelSet allNeededDeepChannels;

    // Reference to the GPU device to process on.
    Blink::ComputeDevice _gpuDevice;
    // Whether to process on the GPU, if available
    bool _useGPUIfAvailable;
    // The amount of gain to apply.
    float _gain;
    // This holds the ProgramSource for the gain kernel.
    Blink::ProgramSource _gainProgram;

    public:

        DeepCBlink(Node* node) : DeepFilterOp(node),
            _outputChannelSet(Mask_RGB),
            _auxiliaryChannelSet(Chan_Black),
            _deepMaskChannel(Chan_Black),
            _deepMaskChannelSet(Chan_Black),
            _rememberedMaskChannel(Chan_Black),
            _sideMaskChannel(Chan_Black),
            _sideMaskChannelSet(Chan_Black),
            _doDeepMask(false),
            _invertDeepMask(false),
            _unpremultDeepMask(true),
            _unpremultPosition(true),
            _premultOutput(true),
            _doSideMask(false),
            _invertSideMask(false),
            _mix(1.0f),
            allNeededDeepChannels(Chan_Black)
            , _gpuDevice(Blink::ComputeDevice::CurrentGPUDevice())
            , _useGPUIfAvailable(true)
            , _gainProgram(GainKernel)
        {
            // defaults mostly go here
            _positionPick[0] = _positionPick[1] = 0.0f;
            _axisKnob.makeIdentity();

            _shape = 0;
            _falloffType = 0;
            _falloff = 1.0f;
            _falloffGamma = 1.0f;

            _gain = 1.0f;
        }

        void findNeededDeepChannels(ChannelSet& neededDeepChannels);
        void _validate(bool);
        bool test_input(int n, Op *op)  const;
        Op* default_input(int input) const;
        const char* input_label(int input, char* buffer) const;
        virtual bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane);
        virtual void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests);
        virtual void knobs(Knob_Callback);
        virtual int knob_changed(DD::Image::Knob* k);

        virtual int minimum_inputs() const { return 2; }
        virtual int maximum_inputs() const { return 2; }
        virtual int optional_input() const { return 1; }
        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};

void DeepCBlink::findNeededDeepChannels(ChannelSet& neededDeepChannels)
{
    neededDeepChannels = Chan_Black;
    neededDeepChannels += _outputChannelSet;
    neededDeepChannels += _auxiliaryChannelSet;
    if (_doDeepMask)
        neededDeepChannels += _deepMaskChannel;
    if (_premultOutput || _unpremultDeepMask)
        neededDeepChannels += Chan_Alpha;
    neededDeepChannels += Chan_DeepBack;
    neededDeepChannels += Chan_DeepFront;
}


void DeepCBlink::_validate(bool for_real)
{

    _falloffGamma = clamp(_falloffGamma, 0.00001f, 65500.0f);

    // make safe the mix
    _mix = clamp(_mix, 0.0f, 1.0f);

    _sideMaskChannelSet = _sideMaskChannel;

    _maskOp = dynamic_cast<Iop*>(Op::input(1));
    if (_maskOp != NULL && _sideMaskChannel != Chan_Black)
    {
        _maskOp->validate(for_real);
        _doSideMask = true;
    } else {
        _doSideMask = false;
    }

    if (_deepMaskChannel != Chan_Black)
    { _doDeepMask = true; }
    else
    { _doDeepMask = false; }

    findNeededDeepChannels(allNeededDeepChannels);

    DeepFilterOp::_validate(for_real);

    // add our output channels to the _deepInfo
    ChannelSet new_channelset;
    new_channelset = _deepInfo.channels();
    new_channelset += _outputChannelSet;
    _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), new_channelset);
}


void DeepCBlink::getDeepRequests(Box bbox, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests)
{
    if (!input0())
        return;

    // make sure we're asking for all required channels
    ChannelSet get_channels = channels;
    get_channels += _auxiliaryChannelSet;
    get_channels += _deepMaskChannel;
    get_channels += Chan_DeepBack;
    get_channels += Chan_DeepFront;
    requests.push_back(RequestData(input0(), bbox, get_channels, count));

    // make sure to request anything we want from the Iop side, too
    if (_doSideMask)
        _maskOp->request(bbox, _sideMaskChannelSet, count);
}

bool DeepCBlink::doDeepEngine(
    Box bbox,
    const DD::Image::ChannelSet& requestedChannels,
    DeepOutputPlane& deepOutPlane
    )
{
    if (!input0())
        return true;

    DD::Image::ChannelSet getChannels = requestedChannels;
    getChannels += allNeededDeepChannels;

    DeepPlane deepInPlane;
    if (!input0()->deepEngine(bbox, getChannels, deepInPlane))
        return false;

    ChannelSet available;
    available = deepInPlane.channels();

    DeepInPlaceOutputPlane inPlaceOutPlane(requestedChannels, bbox);
    int totalSamples = deepInPlane.getTotalSampleCount();
    inPlaceOutPlane.reserveSamples(totalSamples);
    const DD::Image::ChannelSet inputChannels = input0()->deepInfo().channels();
    const int nOutputChans = requestedChannels.size();

    float imageBuffer[totalSamples * 4];
    float outImageBuffer[totalSamples * 4];
    float* imageBufferData;
    float* outImageBufferData;
    imageBufferData = imageBuffer;
    outImageBufferData = outImageBuffer;

    for (int i = 0; i < totalSamples * 4; i++)
    {
        imageBuffer[i] = .05f * (float)i;
    }

    std::vector<float*> outDatas; // vector of pointers to writable data for the channels we're Blink processing
    for (Box::iterator it = bbox.begin(); it != bbox.end(); ++it)
    {
        if (Op::aborted())
            return false; // bail fast on user-interrupt

        // Get the deep pixel from the input plane:
        DeepPixel deepInPixel = deepInPlane.getPixel(it);
        size_t inPixelSamples = deepInPixel.getSampleCount();

        inPlaceOutPlane.setSampleCount(it, deepInPixel.getSampleCount());
        DeepOutputPixel outPixel = inPlaceOutPlane.getPixel(it);

        // for each sample
        for (size_t sampleNo = 0; sampleNo < inPixelSamples; sampleNo++)
        {

            const float* inData = deepInPixel.getUnorderedSample(sampleNo);
            float* outData = outPixel.getWritableUnorderedSample(sampleNo);

            // process the sample
            float input_val;
            int cIndex;
            // for each channel
            foreach(z, getChannels)
            {
                cIndex = colourIndex(z);

                if (_outputChannelSet.contains(z))
                {
                    *imageBufferData = *inData;
                    ++imageBufferData;
                    outDatas.push_back(outData);
                } else {
                    *outData = *inData;
                }
                ++outData;
                ++inData;
            }
        }
    }

    // Now we have the whole input plane in our output plane, and the ChannelSet
    // we want in imageBuffer

    // Has the user requested GPU processing, and is the GPU available for processing on?
    bool usingGPU = _useGPUIfAvailable && _gpuDevice.available();

    // Get a reference to the ComputeDevice to do our processing on.
    // Blink::ComputeDevice computeDevice = usingGPU ? _gpuDevice : Blink::ComputeDevice::CurrentCPUDevice();
    Blink::ComputeDevice computeDevice = Blink::ComputeDevice::CurrentCPUDevice();

    // create a blink image to stuff our buffer into
    Blink::Rect falseRect = {0, 0, totalSamples + 1, 1}; // x1, y1, x2, y2
    Blink::PixelInfo aPixelInfo = {4, kBlinkDataFloat};
    Blink::ImageInfo imageInfo(falseRect, aPixelInfo);

    Blink::Image blinkImage(imageInfo, computeDevice);
    Blink::BufferDesc bufferDesc(sizeof(float) * 4, sizeof(float) * totalSamples * 4, sizeof(float));
    blinkImage.copyFromBuffer(imageBuffer, bufferDesc);

    // Distribute our input image from the device used by NUKE to our ComputeDevice.
    Blink::Image blinkImageOnComputeDevice = blinkImage.distributeTo(computeDevice);

    // This will bind the compute device to the calling thread. This bind should always be performed before
    // beginning any image processing with Blink.
    Blink::ComputeDeviceBinder binder(computeDevice);

    // Make a vector containing the images we want to run the first kernel over.
    std::vector<Blink::Image> images;

    // // The gain kernel requires only the output image as input/output.
    images.clear();
    images.push_back(blinkImageOnComputeDevice);

    // Make a Blink::Kernel from the source in _gainProgram to do the gain.
    Blink::Kernel gainKernel(_gainProgram,
                           computeDevice,
                           images,
                           kBlinkCodegenDefault);
    gainKernel.setParamValue("Gain", _gain);

    // Run the gain kernel over the output image.
    gainKernel.iterate();

    blinkImageOnComputeDevice.copyToBuffer(outImageBuffer, bufferDesc);
    outImageBufferData = outImageBuffer;

    // loop over pointers to writable data we processed in blink
    for (std::vector<float*>::iterator it = outDatas.begin(); it != outDatas.end(); ++it)
    {
        if (Op::aborted())
            return false; // bail fast on user-interrupt
        **it = *outImageBufferData;
        ++outImageBufferData;
    }


    // inPlaceOutPlane.reviseSamples();
    mFnAssert(inPlaceOutPlane.isComplete());
    deepOutPlane = inPlaceOutPlane;
    return true;
}

void DeepCBlink::knobs(Knob_Callback f)
{
    Input_ChannelSet_knob(f, &_auxiliaryChannelSet, 0, "position_data");
    Bool_knob(f, &_unpremultPosition, "unpremult_position_data", "unpremult position");
    Tooltip(f, "Uncheck for ScanlineRender Deep data, check for (probably) "
    "all other renderers. Nuke stores position data from the ScanlineRender "
    "node unpremultiplied, contrary to the Deep spec. Other renderers "
    "presumably store position data (and all other data) premultiplied, "
    "as required by the Deep spec.");
    Input_ChannelSet_knob(f, &_outputChannelSet, 0, "output");
    Bool_knob(f, &_premultOutput, "premult_output", "premult output");
    Tooltip(f, "If, for some reason, you want your mask stored without "
    "premultipling it, contrary to the Deep spec, uncheck this. "
    "Should probably always be checked.");


    Divider(f);
    BeginGroup(f, "Position");
    Axis_knob(f, &_axisKnob, "selection");
    EndGroup(f); // Position

    Divider(f, "");
    Enumeration_knob(f, &_shape, shapeNames, "shape");
    Enumeration_knob(f, &_falloffType, falloffTypeNames, "falloffType");
    Float_knob(f, &_falloff, "falloff");
    Float_knob(f, &_falloffGamma, "falloff_gamma");

    Divider(f, "");

    Input_Channel_knob(f, &_deepMaskChannel, 1, 0, "deep_mask", "deep input mask");
    Bool_knob(f, &_invertDeepMask, "invert_deep_mask", "invert");
    Bool_knob(f, &_unpremultDeepMask, "unpremult_deep_mask", "unpremult");

    Input_Channel_knob(f, &_sideMaskChannel, 1, 1, "side_mask", "side input mask");
    Bool_knob(f, &_invertSideMask, "invert_mask", "invert");
    Float_knob(f, &_mix, "mix");


    Divider(f, "");
    // Add GPU knobs
    Newline(f, "Local GPU: ");
    const bool hasGPU = _gpuDevice.available();
    std::string gpuName = hasGPU ? _gpuDevice.name() : "Not available";
    Named_Text_knob(f, "gpuName", gpuName.c_str());
    Newline(f);
    Bool_knob(f, &_useGPUIfAvailable, "use_gpu", "Use GPU if available");
    Divider(f);

    // Add a parameter for the gain amount.
    Float_knob(f, &_gain, DD::Image::IRange(0, 10), "gain");
    Tooltip(f, "The amount of gain to apply.");

}


int DeepCBlink::knob_changed(DD::Image::Knob* k)
{
    if (k->is("inputChange"))
    {
        // test input 1
        bool input1_connected = dynamic_cast<Iop*>(input(1)) != 0;
        if (!input1_connected)
        {
            _rememberedMaskChannel = _sideMaskChannel;
            knob("side_mask")->set_value(Chan_Black);
        } else
        {
            if (_rememberedMaskChannel == Chan_Black)
                { knob("side_mask")->set_value(Chan_Alpha); }
            else
                { knob("side_mask")->set_value(_rememberedMaskChannel); }
        }
        return 1;
    }

    return DeepFilterOp::knob_changed(k);
}


bool DeepCBlink::test_input(int input, Op *op)  const
{
    switch (input)
    {
        case 0:
            return DeepFilterOp::test_input(input, op);
        case 1:
            return dynamic_cast<Iop*>(op) != 0;
    }
}


Op* DeepCBlink::default_input(int input) const
{
    switch (input)
    {
        case 0:
            return DeepFilterOp::default_input(input);
         case 1:
             Black* dummy;
             return dynamic_cast<Op*>(dummy);
    }
}


const char* DeepCBlink::input_label(int input, char* buffer) const
{
    switch (input)
    {
        case 0: return "";
        case 1: return "mask";
    }
}


const char* DeepCBlink::node_help() const
{
    return
    "Generate noise to use for grading operations.";
}

static Op* build(Node* node) { return new DeepCBlink(node); }
const Op::Description DeepCBlink::d("DeepCBlink", 0, build);
