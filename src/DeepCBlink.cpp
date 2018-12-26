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

/*
TODO: optimizer to use blink or not depending on what's faster - presumably,
if the GPU is available, there's a sample count above which it's better to
use the GPU and below which it's not worth the copying of data
TODO: test on a real machine with a real gpu
TODO: test with more complex function - gain isn't really stressing Blink
TODO: any way to get a bigger chunk of the image to process? or is Deep limited
to working in scanlines. blink would likely be better on larger pieces...
TODO: tidy this up to be a pleasant-looking proof of concept, maybe a grade node
TODO: possible to do a few (dozen) rows at a time, cache the output, and then
other engine calls can just check if this row is cached already and return it?
presumably yes...
TODO:
*/


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
    ChannelSet _processChannelSet;

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
            _processChannelSet(Mask_RGB),
            allNeededDeepChannels(Chan_Black),
            _gpuDevice(Blink::ComputeDevice::CurrentGPUDevice()),
            _useGPUIfAvailable(true),
            _gainProgram(GainKernel)
        {
            _gain = 1.0f;
        }

        void findNeededDeepChannels(ChannelSet& neededDeepChannels);
        void _validate(bool);
        virtual bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane);
        virtual void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests);
        virtual void knobs(Knob_Callback);


        // bool test_input(int n, Op *op)  const;
        // Op* default_input(int input) const;
        // const char* input_label(int input, char* buffer) const;
        // virtual int minimum_inputs() const { return 1; }
        // virtual int maximum_inputs() const { return 1; }
        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};


void DeepCBlink::findNeededDeepChannels(ChannelSet& neededDeepChannels)
{
    neededDeepChannels = Chan_Black;
    neededDeepChannels += _processChannelSet;
    neededDeepChannels += Chan_Alpha;
    neededDeepChannels += Chan_DeepBack;
    neededDeepChannels += Chan_DeepFront;
}


void DeepCBlink::_validate(bool for_real)
{

    findNeededDeepChannels(allNeededDeepChannels);

    DeepFilterOp::_validate(for_real);
}


void DeepCBlink::getDeepRequests(Box bbox, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests)
{
    if (!input0())
        return;

    // make sure we're asking for all required channels
    ChannelSet get_channels = channels;
    get_channels += allNeededDeepChannels;
    requests.push_back(RequestData(input0(), bbox, get_channels, count));
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

    int numChannels;
    if (_processChannelSet.all())
    {
        // handle dealing with multiple channelsets...
        // for now, just bail
        return false;
    }
    numChannels = _processChannelSet.size(); // need to know how many channels we're dealing with so we can tell Blink
    if (numChannels > 4)
    {
        // we could handle this somehow, but for now, just bail
        return false;
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

                if (_processChannelSet.contains(z))
                {
                    *imageBufferData = *inData;
                    ++imageBufferData;
                    outDatas.push_back(outData); // we wil need this pointer later, to write the Blinkified data into
                }
                *outData = *inData;
                ++outData;
                ++inData;
            }
        }
    }

    if (_processChannelSet == Chan_Black)
    {
        // get out if we're not supposed to do anything
        // there's probably a cheaper way to do this
        // inPlaceOutPlane.reviseSamples();
        mFnAssert(inPlaceOutPlane.isComplete());
        deepOutPlane = inPlaceOutPlane;
        return true;
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
    Blink::PixelInfo aPixelInfo = {numChannels, kBlinkDataFloat}; // number of channels, afaict float is correct for second parameter
    Blink::ImageInfo imageInfo(falseRect, aPixelInfo);

    Blink::Image blinkImage(imageInfo, computeDevice);
    // set up a BufferDesc so Blink knows what we're giving it
    // pixelStepBytes: length in bytes to step from one pixel to the next,
    // rowStepBytes: length in bytes from one row to the next (likely only to be one row),
    // componentStepBytes: length in bytes from one componenent to the next
    Blink::BufferDesc bufferDesc(
        sizeof(float) * numChannels,
        sizeof(float) * totalSamples * numChannels,
        sizeof(float));

    // copy our image buffer into the blink image
    blinkImage.copyFromBuffer(imageBuffer, bufferDesc);

    // Distribute our input image from the device used by NUKE to our ComputeDevice.
    Blink::Image blinkImageOnComputeDevice = blinkImage.distributeTo(computeDevice);

    // This will bind the compute device to the calling thread. This bind should always be performed before
    // beginning any image processing with Blink.
    Blink::ComputeDeviceBinder binder(computeDevice);

    // Make a vector containing the images we want to run the first kernel over.
    // if we're passing in multiple images, i.e. multiple inputs to the Blink
    // kernel, we would add them to this vector
    std::vector<Blink::Image> images;
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
        **it = *outImageBufferData; //need to double de-reference the iterator, once to get the pointer, once to get the data pointed to
        ++outImageBufferData;
    }

    // inPlaceOutPlane.reviseSamples();
    mFnAssert(inPlaceOutPlane.isComplete());
    deepOutPlane = inPlaceOutPlane;
    return true;
}

void DeepCBlink::knobs(Knob_Callback f)
{
    Input_ChannelSet_knob(f, &_processChannelSet, 0, "output");

    Divider(f);
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


const char* DeepCBlink::node_help() const
{
    return
    "Proof-of-concept of running Deep through Blink.";
}

static Op* build(Node* node) { return new DeepCBlink(node); }
const Op::Description DeepCBlink::d("DeepCBlink", 0, build);
