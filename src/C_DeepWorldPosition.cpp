#include "DDImage/Iop.h"
#include "DDImage/DeepPixelOp.h"
#include <DDImage/CameraOp.h>
#include "DDImage/Knobs.h"
#include "DDImage/DDMath.h"
#include "DDImage/Matrix4.h"


#include <stdio.h>
#include <math.h>
#include <iostream>

using namespace DD::Image;


static const char* const depthSampleTypeNames[] = { "deep.front", "deep.back", "average", 0 };
static const char* const HELP =
    "Converts Deep data into World Position data in the specified\n"
    "ChannelSet. By default, outputs the data unpremultiplied, but can\n"
    "optionally output premultiplied data (to conform to Deep standards).\n"
    "by: Charles A Taylor";

class C_DeepWorldPosition : public DeepPixelOp
{
    ChannelSet output_channelset;
    int _depthSampleType;
    bool _premultOutput;

    Matrix4 cam_p_mtx;
    Matrix4 camera_world_matrix;

    float hfov;
    float haperture;
    float vaperture;
    float focal_length;

    float win_roll;
    Vector2 win_scale;
    Vector2 win_translate;
    Matrix4 window_matrix;

public:
    C_DeepWorldPosition(Node* node) : DeepPixelOp(node),
    output_channelset(Chan_Black),
    _depthSampleType(0),
    _premultOutput(false)
    {}


    virtual int minimum_inputs() const { return 2; }
    virtual int maximum_inputs() const { return 2; }
    virtual void knobs(Knob_Callback);

    static const Iop::Description d;
    const char* Class() const { return d.name; }
    const char* node_help() const { return HELP; }


    // Wrapper function to work around the "non-virtual thunk" issue on linux when symbol hiding is enabled.
    virtual bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane)
    {
        return DeepPixelOp::doDeepEngine(box, channels, plane);
    }


    // Wrapper function to work around the "non-virtual thunk" issue on linux when symbol hiding is enabled.
    virtual void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests)
    {
        DeepPixelOp::getDeepRequests(box, channels, count, requests);
    }


    void _validate(bool);


    void in_channels(int input, ChannelSet& mask) const {
        switch (_depthSampleType)
        {
            case 0:
                mask += Chan_DeepFront;
                break;
            case 1:
                mask += Chan_DeepBack;
                break;
            case 2:
                mask += Chan_DeepFront;
                mask += Chan_DeepBack;
                break;
        }
        if (_premultOutput)
        {
            // only need alpha if we're premulting
            mask += Chan_Alpha;
        }
        // this may not be required
        mask += output_channelset;
    }

    // TODO: clean
    bool test_input(int n, Op *op)  const {

        if (n >= 1) {
            return dynamic_cast<CameraOp*>(op) != 0;
        }

        return DeepPixelOp::test_input(n, op);
    }

    // TODO: clean
    Op* default_input(int input) const {
        switch (input)
        {
            case 0:
                return DeepPixelOp::default_input(input);
            case 1:
                return CameraOp::default_camera();
        }
    }


    const char* input_label(int input, char* buffer) const {
        switch (input) {
        case 0: return "deep_data";
        case 1: return "cam";
        }
    }

    virtual void processSample(int y,
                                int x,
                                const DD::Image::DeepPixel& deepPixel,
                                size_t sampleNo,
                                const DD::Image::ChannelSet& channels,
                                DeepOutPixel& output
                                ) const;
};

void C_DeepWorldPosition::_validate(bool for_real)
{

    CameraOp* _camOp = dynamic_cast<CameraOp*>(Op::input(1));
    if (_camOp != NULL) {
        _camOp->validate();

        // matrix to transform camera space --> world space
        camera_world_matrix = _camOp->matrix();

        // used to figure out the camera space sample position using trig
        haperture = _camOp->film_width();
        vaperture = _camOp->film_height();
        focal_length = _camOp->focal_length();

        // Matrix to take the camera projection knobs into account
        win_roll = _camOp->win_roll();
        win_scale = _camOp->win_scale();
        win_translate = _camOp->win_translate();
        window_matrix.makeIdentity();
        window_matrix.rotateZ(radians(win_roll));
        window_matrix.scale(1.0f / win_scale[0], 1.0f / win_scale[1.0f], 1.0f);
        window_matrix.translate(-win_translate[0], -win_translate[1], 0.0f);

    }

    DeepPixelOp::_validate(for_real);

    // TODO: test if we need this new ChannelSet or if we can just assign
    //       in the constructor
    // This seems to be what you need to do to add channels to a Deep
    // data stream, as there doesn't seem to be a set_out_channels() method
    // available...
    ChannelSet new_channelset;
    new_channelset = _deepInfo.channels();
    new_channelset += output_channelset;
    _deepInfo = DeepInfo(_deepInfo.formats(), _deepInfo.box(), new_channelset);

    std::cout << "_depthSampleType " << _depthSampleType << "\n";
    std::cout << "_premultOutput" << _premultOutput << "\n";
}

void C_DeepWorldPosition::processSample(
    int y,
    int x,
    const DD::Image::DeepPixel& deepPixel,
    size_t sampleNo,
    const DD::Image::ChannelSet& channels,
    DeepOutPixel& output) const
{

    float uvx;
    float uvy;
    float ndc_x;
    float ndc_y;
    float depth;
    Vector4 camera_space_position;
    Vector4 world_position;
    Matrix4 inverse_window;

    // get pixel location normalized to 0-1 range, store in uvx and uvy
    Op::input_format().to_uv((float)x, (float)y, uvx, uvy);

    ndc_x = uvx * 2.0f - 1.0f;
    ndc_y = uvy * 2.0f - 1.0f;
    switch (_depthSampleType)
    {
        case 0:
            depth = deepPixel.getUnorderedSample(sampleNo, Chan_DeepFront);
            break;
        case 1:
            depth = deepPixel.getUnorderedSample(sampleNo, Chan_DeepBack);
            break;
        case 2:
            depth = (deepPixel.getUnorderedSample(sampleNo, Chan_DeepFront)
                    + deepPixel.getUnorderedSample(sampleNo, Chan_DeepBack))
                    * .5f;
            break;
    }

    // do a little trig to find the camera-space position of the sample
    camera_space_position[0] = ((haperture * 0.5f) / focal_length) * ndc_x * depth;
    camera_space_position[1] = ((vaperture * 0.5f) / focal_length) * ndc_y * depth;
    camera_space_position[2] = -1.0f * depth;
    camera_space_position[3] = 1.0f;

    // takes into account window scale, window roll, window translate
    inverse_window = window_matrix.inverse();
    world_position = camera_world_matrix * inverse_window * camera_space_position;
    if (_premultOutput)
    {
        float alpha;
        alpha = deepPixel.getUnorderedSample(sampleNo, Chan_Alpha);
        world_position *= alpha;
    }
    foreach(z, channels)
    {
        if (output_channelset & z)
        {
            if (colourIndex(z) >= 3)
            {
                // we're outputting XYZ data, if the user asks for a 4th
                // channel, just put 0 in it
                output.push_back(0.0f);
                continue;
            }
            output.push_back(world_position[colourIndex(z)]);
        } else {
            output.push_back(deepPixel.getUnorderedSample(sampleNo, z));
        }
    }
}


void C_DeepWorldPosition::knobs(Knob_Callback f)
{
    ChannelMask_knob(f, &output_channelset, "output_channelset");
    Divider(f, "");
    Enumeration_knob(f, &_depthSampleType, depthSampleTypeNames, "depth_sample");
    Bool_knob(f, &_premultOutput, "premultiplied_output");
    Tooltip(f,  "By default, we output unpremultiplied position\n"
                "data. Check this to output premultiplied position\n"
                "data.");

}


static Op* build(Node* node) { return new C_DeepWorldPosition(node); }
const Op::Description C_DeepWorldPosition::d("C_DeepWorldPosition", 0, build);
