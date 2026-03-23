#pragma once

#include <DDImage/Knob.h>

#include <QWidget>
#include <QGridLayout>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QPainter>
#include <QPen>
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
    void nextCheckState() override;

private:
    QColor _baseColor;
};

/**
 * ArrowLabel — custom QPainter widget for directional arrow indicators.
 *
 * Renders a solid filled triangle arrow centered in a fixed 22x22 cell,
 * matching ChannelButton dimensions exactly. Using a fixed-size custom widget
 * instead of a QLabel with a font glyph prevents QGridLayout column-width
 * inconsistencies caused by variable-width bold text at large point sizes.
 */
class ArrowLabel : public QWidget
{
    Q_OBJECT
public:
    enum Direction { Down, Right };
    explicit ArrowLabel(Direction direction, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Direction _direction;
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
 *   Row 0:  in-group header labels — "in 1" (spanning in1 cols), "const" (spanning 0/1 cols),
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
     * State is stored positionally (row/column indices), not by channel name, so
     * routing is preserved when the user changes a layer picker to a different layer.
     *
     * @param outputGroup   Output group identifier ("out1" or "out2")
     * @param outputRowIdx  Zero-based row index within that output group
     * @param sourceGroup   "in1", "in2", or "const" — which column group was toggled
     * @param sourceColIdx  Zero-based column index within sourceGroup
     *                      (for "const": 0 = 0.0, 1 = 1.0)
     * @param checked       true = route source -> output; false = no-op (radio guard)
     */
    void onCellToggled(const std::string& outputGroup,
                       int outputRowIdx,
                       const std::string& sourceGroup,
                       int sourceColIdx,
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
     * Object names use "groupId|outputChannelName|inputGroup|sourceChannelName" format.
     * ChannelButton inherits QPushButton — all QPushButton method calls remain valid.
     */
    std::vector<ChannelButton*> _toggleButtons;

    /**
     * ChannelSet picker QComboBoxes — one per input/output group.
     * Created in buildLayout() and deleted in clearLayout() via the grid layout.
     * Null between clearLayout() and the next buildLayout() call.
     *
     * Signal handlers use QTimer::singleShot(0, ...) to defer all Nuke API
     * calls to the next event loop tick, preventing re-entrant destruction of
     * the widget while it is still inside a currentTextChanged signal handler.
     */
    QComboBox* _in1Picker  = nullptr;
    QComboBox* _in2Picker  = nullptr;
    QComboBox* _out1Picker = nullptr;
    QComboBox* _out2Picker = nullptr;
};
