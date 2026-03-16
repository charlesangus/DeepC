#include "ShuffleMatrixKnob.h"
#include "ShuffleMatrixWidget.h"
#include "DDImage/Channel.h"
#include "DDImage/Op.h"
#include <ostream>
#include <sstream>
#include <set>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Knob identity
// ---------------------------------------------------------------------------

const char* ShuffleMatrixKnob::Class() const
{
    return "ShuffleMatrixKnob";
}

// ---------------------------------------------------------------------------
// Serialization virtuals
// ---------------------------------------------------------------------------

bool ShuffleMatrixKnob::not_default() const
{
    return !_matrixState.empty();
}

void ShuffleMatrixKnob::to_script(std::ostream& outputStream,
                                   const DD::Image::OutputContext* /*outputContext*/,
                                   bool quote) const
{
    if (quote)
        outputStream << cstring(_matrixState.c_str());
    else
        outputStream << _matrixState;
}

bool ShuffleMatrixKnob::from_script(const char* serializedValue)
{
    _matrixState = serializedValue ? serializedValue : "";
    changed(); // schedules kUpdateWidgets so the widget syncs when it next opens
    return true;
}

void ShuffleMatrixKnob::store(DD::Image::StoreType /*storeType*/,
                               void* dest,
                               DD::Image::Hash& hash,
                               const DD::Image::OutputContext& /*outputContext*/)
{
    if (dest)
        *static_cast<std::string*>(dest) = _matrixState;
    hash.append(_matrixState);
}

// ---------------------------------------------------------------------------
// Widget factory
// ---------------------------------------------------------------------------

WidgetPointer ShuffleMatrixKnob::make_widget(const DD::Image::WidgetContext& /*widgetContext*/)
{
    ShuffleMatrixWidget* widget = new ShuffleMatrixWidget(this);
    // Store a direct pointer to the widget so syncWidgetNow() can call
    // syncFromKnob() synchronously without relying on the async updateWidgets()
    // callback queue. The widget nulls this pointer in its kDestroying callback.
    _widget = widget;
    // knob_changed(showPanel) fires BEFORE make_widget is called, so the widget
    // is constructed with whatever ChannelSets are current. Force an immediate
    // syncFromKnob() here to ensure column headers reflect live state from the
    // moment the panel opens — compensates for the showPanel timing gap.
    widget->syncFromKnob();
    return widget;
}

// ---------------------------------------------------------------------------
// Public accessors
// ---------------------------------------------------------------------------

void ShuffleMatrixKnob::setValue(const std::string& newState)
{
    new_undo("ShuffleMatrix");
    _matrixState = newState;
    changed();
}

const std::string& ShuffleMatrixKnob::matrixState() const
{
    return _matrixState;
}

void ShuffleMatrixKnob::initializeState(const std::string& state)
{
    _matrixState = state;
    // Intentionally does NOT call changed() — avoids recursive rebuild.
    // Caller (syncFromKnob) re-syncs button states inline after calling this.
}

void ShuffleMatrixKnob::syncWidgetNow()
{
    if (_widget)
        _widget->syncFromKnob();
}

void ShuffleMatrixKnob::clearWidgetPointer()
{
    _widget = nullptr;
}

void ShuffleMatrixKnob::setChannelSets(const DD::Image::ChannelSet& in1ChannelSet,
                                        const DD::Image::ChannelSet& in2ChannelSet,
                                        const DD::Image::ChannelSet& out1ChannelSet,
                                        const DD::Image::ChannelSet& out2ChannelSet)
{
    // Store all four channel sets so the widget can use them to build column
    // headers and row labels. Do NOT call changed() here — this is a UI-only
    // update and the caller is responsible for triggering updateWidgets().
    _in1ChannelSet  = in1ChannelSet;
    _in2ChannelSet  = in2ChannelSet;
    _out1ChannelSet = out1ChannelSet;
    _out2ChannelSet = out2ChannelSet;
}

const DD::Image::ChannelSet& ShuffleMatrixKnob::in1ChannelSet() const
{
    return _in1ChannelSet;
}

const DD::Image::ChannelSet& ShuffleMatrixKnob::in2ChannelSet() const
{
    return _in2ChannelSet;
}

const DD::Image::ChannelSet& ShuffleMatrixKnob::out1ChannelSet() const
{
    return _out1ChannelSet;
}

const DD::Image::ChannelSet& ShuffleMatrixKnob::out2ChannelSet() const
{
    return _out2ChannelSet;
}

std::vector<std::string> ShuffleMatrixKnob::availableLayerNames() const
{
    std::vector<std::string> layerNames;
    layerNames.push_back("none");  // always first: Chan_Black / no channels

    // Collect unique layer names from all channels known to the NDK.
    // DD::Image::Chan_Last is the upper bound of defined channels.
    // We iterate all defined channels and extract the layer (prefix before '.').
    std::set<std::string> seen;
    for (int channelIndex = 1; channelIndex < DD::Image::Chan_Last; ++channelIndex)
    {
        const DD::Image::Channel channel = static_cast<DD::Image::Channel>(channelIndex);
        const char* fullName = DD::Image::getName(channel);
        if (!fullName || fullName[0] == '\0')
            continue;

        const std::string fullNameStr(fullName);
        const std::string::size_type dotPos = fullNameStr.rfind('.');
        if (dotPos == std::string::npos)
            continue;

        const std::string layerName = fullNameStr.substr(0, dotPos);
        if (layerName.empty() || layerName == "other")
            continue;

        if (seen.insert(layerName).second)
            layerNames.push_back(layerName);
    }

    return layerNames;
}
