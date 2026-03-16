#pragma once

#include <DDImage/Knob.h>
#include <DDImage/Hash.h>
#include <DDImage/OutputContext.h>
#include <DDImage/ChannelSet.h>
#include <string>
#include <sstream>
#include <array>

// Forward declare the Qt widget — do not include ShuffleMatrixWidget.h here
// to avoid a circular dependency (widget header includes this header for knob pointer).
class ShuffleMatrixWidget;

/**
 * ShuffleMatrixKnob — custom DD::Image::Knob subclass for DeepCShuffle.
 *
 * Embeds a ShuffleMatrixWidget (Qt toggle-button grid) inside the Nuke node
 * panel. Routing state is serialized to a flat string via to_script/from_script
 * so it survives script save/load and headless renders with no Qt dependency.
 *
 * Registration in Op::knobs():
 *   CustomKnob2(ShuffleMatrixKnob, f, &_matrixState, "matrix", "routing");
 *
 * The store() override copies _matrixState into the Op's std::string member
 * and appends it to the recook hash so engine invalidation works correctly.
 */

// Serialization format: comma-separated "outputChannelName:sourceChannelName" pairs.
// Example: "rgba.red:rgba.red,rgba.green:depth.Z"
// Channels absent from the string pass through unchanged (not zeroed).
// Channel names use the NDK getName(Channel) format: "layerName.channelName".
// Special source names "const:0" and "const:1" represent constant 0.0 and 1.0.

class ShuffleMatrixKnob : public DD::Image::Knob
{
public:
    /**
     * Constructor — signature required by the CustomKnob2 macro.
     * Initialises _matrixState from the Op's backing std::string if data is
     * non-null (happens during script load before from_script is called).
     */
    ShuffleMatrixKnob(DD::Image::Knob_Closure* kc,
                      std::string* data,
                      const char* name,
                      const char* label)
        : Knob(kc, name, label)
    {
        if (data) {
            _matrixState = *data;
        }
    }

    // -------------------------------------------------------------------------
    // Knob identity
    // -------------------------------------------------------------------------

    const char* Class() const override;

    // -------------------------------------------------------------------------
    // Serialization virtuals — required by Nuke script read/write and undo
    // -------------------------------------------------------------------------

    /** Returns true when the routing state is non-empty (non-default). */
    bool not_default() const override;

    /**
     * Writes the current matrix state to the script output stream.
     * When quote == true, the string is wrapped in Tcl quoting via cstring().
     */
    void to_script(std::ostream& outputStream,
                   const DD::Image::OutputContext* outputContext,
                   bool quote) const override;

    /**
     * Reads the matrix state back from a script string.
     * Calls changed() so Nuke schedules a kUpdateWidgets callback to sync
     * any open widget after the script is loaded.
     */
    bool from_script(const char* serializedValue) override;

    /**
     * Copies _matrixState into the Op's backing storage and appends it to
     * the recook hash so the engine re-runs when routing changes.
     */
    void store(DD::Image::StoreType storeType,
               void* dest,
               DD::Image::Hash& hash,
               const DD::Image::OutputContext& outputContext) override;

    // -------------------------------------------------------------------------
    // Widget factory — called by Nuke to build the panel UI
    // -------------------------------------------------------------------------

    /**
     * Creates and returns a ShuffleMatrixWidget.
     * Nuke owns the returned pointer; do not delete it manually.
     */
    WidgetPointer make_widget(const DD::Image::WidgetContext& widgetContext) override;

    // -------------------------------------------------------------------------
    // Public accessors — called by the widget and by the Op
    // -------------------------------------------------------------------------

    /**
     * Called by ShuffleMatrixWidget when the user toggles a routing cell.
     * Registers an undo checkpoint, updates _matrixState, and notifies Nuke
     * that the knob value has changed (triggering a recook hash update).
     */
    void setValue(const std::string& newState);

    /** Read-only accessor used by the widget to read current routing state. */
    const std::string& matrixState() const;

    /**
     * Writes state directly without calling changed() — for identity initialisation only.
     * Caller must re-sync button states manually after calling this.
     */
    void initializeState(const std::string& state);

    /**
     * Called by Op::knob_changed() to push all four ChannelSets into the knob
     * so that make_widget() and syncFromKnob() can build accurate headers and
     * row labels without calling back into the Op.
     */
    void setChannelSets(const DD::Image::ChannelSet& in1ChannelSet,
                        const DD::Image::ChannelSet& in2ChannelSet,
                        const DD::Image::ChannelSet& out1ChannelSet,
                        const DD::Image::ChannelSet& out2ChannelSet);

    const DD::Image::ChannelSet& in1ChannelSet() const;
    const DD::Image::ChannelSet& in2ChannelSet() const;
    const DD::Image::ChannelSet& out1ChannelSet() const;
    const DD::Image::ChannelSet& out2ChannelSet() const;

private:
    // Serialization format: comma-separated "outputChannelName:sourceChannelName" pairs.
    // Example: "rgba.red:rgba.red,rgba.green:depth.Z"
    // Channels absent from the string pass through unchanged (not zeroed).
    // Channel names use the NDK getName(Channel) format: "layerName.channelName".
    std::string _matrixState;

    DD::Image::ChannelSet _in1ChannelSet;
    DD::Image::ChannelSet _in2ChannelSet;
    DD::Image::ChannelSet _out1ChannelSet;
    DD::Image::ChannelSet _out2ChannelSet;
};
