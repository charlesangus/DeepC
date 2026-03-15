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
 * Displays a toggle-button matrix that matches the reference Shuffle node UI:
 *   - Columns = source channels (in1 channels, then "0"/"1" constants, then in2 channels)
 *   - Rows    = output channels drawn from out1 ChannelSet (first group) and
 *               out2 ChannelSet (second group, if out2 != Chan_Black)
 *
 * Each toggle button at [row, col] routes the corresponding source column
 * channel into that row's output channel. Clicking a button calls
 * ShuffleMatrixKnob::setValue() with the updated serialized routing string.
 *
 * Special source columns:
 *   "const:0" — constant 0.0 value
 *   "const:1" — constant 1.0 value
 *
 * Grid layout structure (row indices are zero-based):
 *   Row 0:  in-group header labels — blank, "in 1" (spanning in1 cols), "0", "1",
 *             "in 2" (spanning in2 cols if active), blank for output label column
 *   Row 1:  channel name labels (r, g, b, a …) + "0", "1" + in2 channel names
 *   Row 2:  down-arrow labels under each source column
 *   Row 3+: one toggle-button row per output channel; output channel name label on right
 *
 * Lifecycle is managed via Knob::addCallback / removeCallback. The static
 * WidgetCallback handles kUpdateWidgets (rebuilds layout from current knob
 * state), kDestroying (nullifies the _knob pointer so no UAF occurs), and
 * kIsVisible (returns visibility to Nuke's panel manager).
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
     * @param sourceChannelName  NDK channel name for the source (e.g. "depth.Z"),
     *                           or "const:0" / "const:1" for constant columns
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
     * See class documentation for the row/column structure.
     * Sets a minimum height hint based on row count to ensure Nuke's panel allocates space.
     */
    void buildLayout();

    /**
     * Removes all child widgets from _gridLayout and clears all tracking vectors.
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
     * Flat list of all toggle QPushButtons in row-major order.
     * Object names use "outputChannelName|sourceChannelName" format for state sync.
     */
    std::vector<QPushButton*> _toggleButtons;

    /**
     * Ordered list of output channel names (NDK format: "layerName.channelName")
     * for the rows currently shown. Drawn from out1 then out2 ChannelSets.
     */
    std::vector<std::string> _outputChannelNames;

    /**
     * Ordered list of source column identifiers for the columns currently shown.
     * Format: NDK channel name (e.g. "rgba.red") or "const:0" / "const:1".
     * Order: in1 channels, then "const:0", "const:1", then in2 channels (if active).
     */
    std::vector<std::string> _sourceColumnNames;
};
