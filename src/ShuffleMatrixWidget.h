#pragma once

#include <DDImage/Knob.h>

#include <QWidget>
#include <QGridLayout>
#include <QPushButton>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QComboBox>
#include <vector>
#include <string>

// Forward declare the knob — do not include ShuffleMatrixKnob.h at the class
// declaration level to avoid a circular include dependency.
// The .cpp implementation file includes both headers.
class ShuffleMatrixKnob;

// Returns the Nuke-standard display color for a short channel name (e.g. "red", "g", "alpha").
QColor nukeChannelColor(const std::string& shortChannelName);

/**
 * ChannelButton — QPushButton subclass used in the ShuffleMatrixWidget.
 *
 * Renders a colored square that matches the Nuke channel color convention.
 * When checked, draws a white X mark across the button face via paintEvent.
 * When disabled, renders in a neutral dark grey regardless of channel color.
 */
class ChannelButton : public QPushButton
{
    Q_OBJECT
public:
    explicit ChannelButton(QColor baseColor, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor _baseColor;
};

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
     * @param outputGroup        Output group identifier (e.g. "out1", "out2", layer name)
     * @param outputChannelName  NDK channel name for the destination slot (e.g. "rgba.red")
     * @param inputGroup         "in1" or "in2" — which column group was toggled
     * @param sourceChannelName  NDK channel name for the source (e.g. "depth.Z")
     * @param checked            true = route source -> output; false = remove routing
     */
    void onCellToggled(const std::string& outputGroup,
                       const std::string& outputChannelName,
                       const std::string& inputGroup,
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
     * Flat list of all toggle ChannelButtons in row-major order.
     * Object names use "outputChannelName|inputGroup|sourceChannelName" format for state sync.
     * ChannelButton inherits QPushButton — all QPushButton method calls remain valid.
     */
    std::vector<ChannelButton*> _toggleButtons;

    /**
     * Embedded layer-picker QComboBoxes displayed inside the matrix widget header rows.
     * in1/in2 pickers appear above their respective column groups; out1/out2 pickers
     * appear to the right of their respective row groups.
     *
     * These pointers are set to nullptr by clearLayout() (the widgets are deleted by
     * the layout cleanup) and recreated fresh in each buildLayout() call.
     */
    QComboBox* _in1Picker;
    QComboBox* _in2Picker;
    QComboBox* _out1Picker;
    QComboBox* _out2Picker;

    /**
     * Populates a layer-picker QComboBox with known layer names and selects the
     * layer currently shown by the given layer name string.
     *
     * @param picker            The QComboBox to populate.
     * @param currentLayerName  The layer name to pre-select (e.g. "rgba", "depth").
     *                          Pass an empty string to default to "none".
     */
    void populatePicker(QComboBox* picker, const std::string& currentLayerName);

    /**
     * Called when a picker's selection changes. Updates the corresponding NDK
     * ChannelSet knob on the Op and triggers knob_changed so the matrix columns/rows
     * rebuild immediately.
     *
     * @param knobName      NDK knob name: "in1", "in2", "out1", or "out2".
     * @param selectedLayer Layer name chosen in the picker (e.g. "rgba", "depth").
     *                      "none" maps to an empty string passed to set_text().
     */
    void onPickerChanged(const std::string& knobName, const std::string& selectedLayer);
};
