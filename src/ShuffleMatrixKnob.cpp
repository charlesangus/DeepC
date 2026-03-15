#include "ShuffleMatrixKnob.h"
#include "ShuffleMatrixWidget.h"
#include "DDImage/Channel.h"
#include <ostream>
#include <sstream>

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
    return new ShuffleMatrixWidget(this);
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

void ShuffleMatrixKnob::setChannelSets(const DD::Image::ChannelSet& in1ChannelSet,
                                        const DD::Image::ChannelSet& in2ChannelSet)
{
    // Store the channel sets so the widget can use them to build column headers.
    // Do NOT call changed() here — this is a UI-only update and the caller is
    // responsible for triggering updateWidgets() on the knob if needed.
    _in1ChannelSet = in1ChannelSet;
    _in2ChannelSet = in2ChannelSet;
}

const DD::Image::ChannelSet& ShuffleMatrixKnob::in1ChannelSet() const
{
    return _in1ChannelSet;
}

const DD::Image::ChannelSet& ShuffleMatrixKnob::in2ChannelSet() const
{
    return _in2ChannelSet;
}
