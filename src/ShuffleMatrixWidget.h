#pragma once

#include <DDImage/Knob.h>

#include <QWidget>
#include <QGridLayout>
#include <QPushButton>
#include <QLabel>
#include <vector>
#include <string>

// Forward declare the knob — do not include ShuffleMatrixKnob.h at the class
// declaration level to avoid a circular include dependency.
// The .cpp implementation file includes both headers.
class ShuffleMatrixKnob;

/**
 * ShuffleMatrixWidget — Qt widget embedded in the DeepCShuffle node panel.
 *
 * Displays a toggle-button matrix where:
 *   - Rows  = output channels drawn from the in1 ChannelSet (up to 8)
 *   - Cols  = source channels from both in1 and in2 ChannelSets
 *
 * Each toggle button at [row, col] routes the corresponding source channel
 * into the output slot. Clicking a button calls ShuffleMatrixKnob::setValue()
 * with the updated serialized routing string.
 *
 * Lifecycle is managed via Knob::addCallback / removeCallback. The static
 * WidgetCallback handles kUpdateWidgets (rebuilds layout from current knob
 * state), kDestroying (nullifies the _knob pointer so no UAF occurs), and
 * kIsVisible (returns visibility to Nuke's panel manager).
 *
 * Grid layout structure:
 *   Row 0: header labels — col 0 empty, col 1..N source channel names
 *   Row 1..8: output channel label (col 0) + toggle buttons (col 1..N)
 *   Rows = output channels from in1 ChannelSet (<=8 shown)
 *   Cols = source channels from in1 + in2 ChannelSets
 */
class ShuffleMatrixWidget : public QWidget
{
    Q_OBJECT

public:
    /**
     * Constructs the widget, registers the Knob lifecycle callback, and
     * calls buildLayout() to populate the initial grid.
     */
    explicit ShuffleMatrixWidget(ShuffleMatrixKnob* knob, QWidget* parent = nullptr);

    /**
     * Removes the Knob lifecycle callback (if _knob is still valid) before
     * the widget is destroyed.
     */
    ~ShuffleMatrixWidget();

    /**
     * Static callback registered with ShuffleMatrixKnob via addCallback().
     * Dispatches on reason:
     *   kIsVisible    — returns whether this widget is visible in its panel
     *   kUpdateWidgets — calls syncFromKnob() to rebuild layout from knob state
     *   kDestroying   — sets _knob = nullptr to prevent use-after-free
     */
    static int WidgetCallback(void* closure, DD::Image::Knob::CallbackReason reason);

public slots:
    /**
     * Called when the user toggles a routing cell button.
     * Serialises the new routing state and forwards it to ShuffleMatrixKnob::setValue().
     *
     * @param outputChannelName  NDK channel name for the destination slot (e.g. "rgba.red")
     * @param sourceChannelName  NDK channel name for the source (e.g. "depth.Z")
     * @param checked            true = route source -> output; false = remove routing
     */
    void onCellToggled(const std::string& outputChannelName,
                       const std::string& sourceChannelName,
                       bool checked);

    /**
     * Rebuilds column headers from the current ChannelSets stored in _knob
     * and re-syncs all toggle button states from the current matrixState.
     * Called from WidgetCallback when kUpdateWidgets fires.
     */
    void syncFromKnob();

private:
    /**
     * Creates the QGridLayout with column header QLabels and toggle QPushButtons.
     * Row 0: empty label + source channel name labels.
     * Rows 1-8: output channel name label + one checkable QPushButton per source channel.
     * Sets a minimum height hint based on row count to ensure Nuke's panel allocates space.
     */
    void buildLayout();

    /**
     * Removes all child widgets from _gridLayout and clears the _toggleButtons,
     * _outputChannelNames, and _sourceChannelNames tracking vectors.
     * Called at the start of syncFromKnob() before rebuilding.
     */
    void clearLayout();

    // -------------------------------------------------------------------------
    // Private members
    // -------------------------------------------------------------------------

    /** Raw pointer to the owning knob. Set to nullptr in kDestroying callback. */
    ShuffleMatrixKnob* _knob;

    /** The grid layout that holds header labels and toggle buttons. */
    QGridLayout* _gridLayout;

    /**
     * Flat list of all toggle QPushButtons in row-major order
     * (row0_col0, row0_col1, ..., row1_col0, ...).
     * Used for efficient state synchronisation in syncFromKnob().
     */
    std::vector<QPushButton*> _toggleButtons;

    /**
     * Ordered list of output channel names (NDK format: "layerName.channelName")
     * for the rows currently shown (up to 8, from in1 ChannelSet).
     */
    std::vector<std::string> _outputChannelNames;

    /**
     * Ordered list of source channel names (NDK format) for the columns
     * currently shown (in1 channels first, then in2 channels).
     */
    std::vector<std::string> _sourceChannelNames;
};
