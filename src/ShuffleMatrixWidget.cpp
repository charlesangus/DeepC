#include "ShuffleMatrixWidget.h"
#include "ShuffleMatrixKnob.h"
#include "DDImage/Channel.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Op.h"
#include <QTabWidget>
#include <QTimer>
#include <sstream>
#include <map>

// Returns the part of a full NDK channel name after the last dot ("rgba.red" -> "red").
static std::string shortChannelName(const std::string& fullName)
{
    const std::string::size_type dotPosition = fullName.rfind('.');
    if (dotPosition != std::string::npos)
        return fullName.substr(dotPosition + 1);
    return fullName;
}

// Returns the layer part of a full NDK channel name ("rgba.red" -> "rgba").
static std::string layerNameFromChannelSet(const DD::Image::ChannelSet& channelSet)
{
    DD::Image::Channel ch;
    foreach(ch, channelSet)
    {
        const std::string fullName = DD::Image::getName(ch);
        const std::string::size_type dotPosition = fullName.rfind('.');
        if (dotPosition != std::string::npos)
            return fullName.substr(0, dotPosition);
        return fullName;
    }
    return "";
}

// ---------------------------------------------------------------------------
// nukeChannelColor — maps short channel name to Nuke-standard display color
// ---------------------------------------------------------------------------

QColor nukeChannelColor(const std::string& shortChannelName)
{
    if (shortChannelName == "red"   || shortChannelName == "r")  return QColor(180,  45,  45);
    if (shortChannelName == "green" || shortChannelName == "g")  return QColor( 45, 140,  45);
    if (shortChannelName == "blue"  || shortChannelName == "b")  return QColor( 45,  90, 180);
    if (shortChannelName == "alpha" || shortChannelName == "a")  return QColor(155, 155, 155);
    if (shortChannelName == "z"     || shortChannelName == "Z")  return QColor(180,  45,  45);
    if (shortChannelName == "u")                                 return QColor(160,  45, 160);
    if (shortChannelName == "v")                                 return QColor(200,  80, 200);
    if (shortChannelName == "x")                                 return QColor(130,  45, 170);
    if (shortChannelName == "y")                                 return QColor(100,  45, 150);
    if (shortChannelName == "w")                                 return QColor( 80,  45, 130);
    return QColor(80, 80, 80);
}

// ---------------------------------------------------------------------------
// ChannelButton — constructor and paintEvent
// ---------------------------------------------------------------------------

ChannelButton::ChannelButton(QColor baseColor, QWidget* parent)
    : QPushButton(parent)
    , _baseColor(baseColor)
{
    setCheckable(true);
    setFixedSize(22, 22);
    setFlat(true);
}

void ChannelButton::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QColor fillColor = isEnabled() ? _baseColor : QColor(55, 55, 55);
    if (isChecked() && isEnabled())
        fillColor = fillColor.lighter(130);

    painter.fillRect(rect(), fillColor);

    painter.setPen(QPen(QColor(25, 25, 25), 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));

    if (isChecked() && isEnabled())
    {
        // Choose X-mark color based on fill luminance to ensure visibility on
        // both dark and light backgrounds. const:1 buttons have a near-white
        // fill (lightnessF > 0.5) so the X must be drawn in black, not white.
        const QColor xMarkColor = (fillColor.lightnessF() > 0.5) ? Qt::black : Qt::white;
        painter.setPen(QPen(xMarkColor, 2));
        painter.drawLine(4, 4, 17, 17);
        painter.drawLine(17, 4, 4, 17);
    }
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

ShuffleMatrixWidget::ShuffleMatrixWidget(ShuffleMatrixKnob* knob, QWidget* parent)
    : QWidget(parent)
    , _knob(knob)
    , _gridLayout(nullptr)
{
    _knob->addCallback(WidgetCallback, this);
    _gridLayout = new QGridLayout(this);
    _gridLayout->setHorizontalSpacing(2);
    _gridLayout->setVerticalSpacing(2);
    _gridLayout->setContentsMargins(4, 4, 4, 4);
    buildLayout();
}

ShuffleMatrixWidget::~ShuffleMatrixWidget()
{
    if (_knob)
        _knob->removeCallback(WidgetCallback, this);
}

// ---------------------------------------------------------------------------
// Static callback — handles Nuke knob lifecycle events
// ---------------------------------------------------------------------------

int ShuffleMatrixWidget::WidgetCallback(void* closure, DD::Image::Knob::CallbackReason reason)
{
    auto* widget = static_cast<ShuffleMatrixWidget*>(closure);
    switch (reason)
    {
        case DD::Image::Knob::kIsVisible:
            for (QWidget* parentWidget = widget->parentWidget();
                 parentWidget;
                 parentWidget = parentWidget->parentWidget())
            {
                if (qobject_cast<QTabWidget*>(parentWidget))
                    return widget->isVisibleTo(parentWidget);
            }
            return widget->isVisible() ? 1 : 0;

        case DD::Image::Knob::kUpdateWidgets:
            widget->syncFromKnob();
            return 0;

        case DD::Image::Knob::kDestroying:
            // Null the back-pointer in the knob so syncWidgetNow() cannot fire
            // into a destroyed widget. Must come before nulling widget->_knob
            // because clearWidgetPointer() dereferences the knob pointer.
            if (widget->_knob)
                widget->_knob->clearWidgetPointer();
            widget->_knob = nullptr;
            return 0;

        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// Layout construction
// ---------------------------------------------------------------------------

void ShuffleMatrixWidget::buildLayout()
{
    const DD::Image::ChannelSet& in1Set  = _knob->in1ChannelSet();
    const DD::Image::ChannelSet& in2Set  = _knob->in2ChannelSet();
    const DD::Image::ChannelSet& out1Set = _knob->out1ChannelSet();
    const DD::Image::ChannelSet& out2Set = _knob->out2ChannelSet();

    // ---- Collect input source columns ----

    std::vector<std::string> in1Columns;
    std::vector<std::string> in2Columns;

    {
        DD::Image::Channel ch;
        foreach(ch, in1Set)
            in1Columns.push_back(DD::Image::getName(ch));
    }

    // When in2 is set to none (Chan_Black) we still want to render the in2 columns
    // but with all buttons disabled, so the layout does not visually collapse.
    // Populate placeholder channel names so the column group is always visible.
    const bool in2Disabled = (in2Set == DD::Image::ChannelSet(DD::Image::Chan_Black));
    {
        DD::Image::Channel ch;
        foreach(ch, in2Set)
            in2Columns.push_back(DD::Image::getName(ch));
    }
    if (in2Disabled && in2Columns.empty())
    {
        // Insert fixed placeholder column names so the in2 section always renders
        // as a visible greyed-out group even when no layer is selected.
        in2Columns.push_back("rgba.red");
        in2Columns.push_back("rgba.green");
        in2Columns.push_back("rgba.blue");
        in2Columns.push_back("rgba.alpha");
    }

    // ---- Collect output rows per group ----

    std::vector<std::string> out1Rows;
    std::vector<std::string> out2Rows;

    {
        DD::Image::Channel ch;
        foreach(ch, out1Set)
        {
            if (out1Rows.size() >= 8) break;
            out1Rows.push_back(DD::Image::getName(ch));
        }
    }

    // When out2 is set to none (Chan_Black) we still want to render the out2 rows
    // but with all buttons disabled, so the layout does not visually collapse.
    const bool out2Disabled = (out2Set == DD::Image::ChannelSet(DD::Image::Chan_Black));
    {
        DD::Image::Channel ch;
        foreach(ch, out2Set)
        {
            if (out2Rows.size() >= 8) break;
            out2Rows.push_back(DD::Image::getName(ch));
        }
    }

    // ---- Shared column layout ----
    //
    //   Cols 0 .. in1Count-1           : in1 toggle columns
    //   Col  in1Count                  : "0" const column (constant 0.0 source)
    //   Col  in1Count+1                : "1" const column (constant 1.0 source)
    //   Cols in1Count+2 .. +in2Count+1 : in2 toggle columns (always rendered; disabled if in2=none)
    //   Col  outLabelCol               : output channel name / group name label
    //
    // No spacer column between const and in2 — column groups flow directly into each other.

    const int in1Count     = static_cast<int>(in1Columns.size());
    const int in2Count     = static_cast<int>(in2Columns.size());
    const int const0Col    = in1Count;
    const int const1Col    = in1Count + 1;
    const int in2StartCol  = in1Count + 2;
    const int outLabelCol  = in2StartCol + in2Count;

    _toggleButtons.clear();

    // ---- Set column stretch so button columns stay fixed-width ----
    // All button and label-header columns are fixed to their content width
    // (buttons are 22x22px). The output label column on the right gets all
    // remaining stretch so it expands without pushing the button columns apart.
    for (int col = 0; col < outLabelCol; ++col)
        _gridLayout->setColumnStretch(col, 0);
    _gridLayout->setColumnStretch(outLabelCol, 1);

    // ---- Helper: create a down-arrow label with enlarged font ----
    // ↓ (U+2193) rendered at a larger point size so the arrow is clearly visible
    // even at the small column widths used for 22px toggle buttons.
    auto makeArrowLabel = [&](const QString& arrowChar) -> QLabel*
    {
        QLabel* arrow = new QLabel(arrowChar, this);
        arrow->setAlignment(Qt::AlignCenter);
        QFont arrowFont = arrow->font();
        arrowFont.setPointSize(arrowFont.pointSize() + 4);
        arrow->setFont(arrowFont);
        return arrow;
    };

    // ---- Helper: create and populate a ChannelSet picker QComboBox ----
    //
    // Creates a QComboBox populated with all available layer names from the knob,
    // sets the current selection to currentLayerName, and connects currentTextChanged
    // to a QTimer::singleShot(0) deferred handler that calls the supplied callback.
    // The deferred callback breaks the re-entrant call stack that caused a segfault
    // in the previous QComboBox attempt (commit 8785a83): the Nuke API calls that
    // rebuild the widget are deferred to the next event loop tick, so the signal
    // handler returns before the QComboBox that fired it can be deleted.

    auto makePicker = [&](const std::string& currentLayerName,
                          std::function<void(const QString&)> onChanged) -> QComboBox*
    {
        QComboBox* picker = new QComboBox(this);
        picker->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        const std::vector<std::string> layerNames = _knob->availableLayerNames();
        int currentIndex = 0;
        for (int idx = 0; idx < static_cast<int>(layerNames.size()); ++idx)
        {
            picker->addItem(QString::fromStdString(layerNames[idx]));
            if (layerNames[idx] == currentLayerName)
                currentIndex = idx;
        }
        // If currentLayerName was not in the list (e.g. a non-standard layer),
        // add it so the current state is always visible in the picker.
        if (currentIndex == 0 && !currentLayerName.empty() && currentLayerName != "none")
        {
            picker->addItem(QString::fromStdString(currentLayerName));
            currentIndex = picker->count() - 1;
        }
        {
            const QSignalBlocker blockWhileSetting(picker);
            picker->setCurrentIndex(currentIndex);
        }

        connect(picker, &QComboBox::currentTextChanged,
                [onChanged](const QString& selectedText)
                {
                    // Defer the Nuke API call to the next event loop tick via
                    // QTimer::singleShot(0). This ensures the currentTextChanged
                    // signal dispatch completes and the QComboBox remains alive
                    // before syncFromKnob() rebuilds (and deletes) the widget.
                    QTimer::singleShot(0, [onChanged, selectedText]()
                    {
                        onChanged(selectedText);
                    });
                });

        return picker;
    };

    // ---- Lambda: build one output group (two header rows + data rows) ----
    //
    // Each group gets its own "in 1" / "in 2" header and per-column channel
    // labels so it reads as a self-contained matrix, not part of a shared block.
    // Returns the next unused grid row.
    //
    // groupId    — stable identity string used in button objectNames for radio
    //              enforcement ("out1" or "out2"). Must be unique across groups.
    // groupLabel — human-readable display name shown in the panel (layer name
    //              or "out 1"/"out 2" fallback). Independent of groupId.
    // outPickerPtr — receives the created output picker QComboBox; used by
    //                buildLayout() to store the pointer into _out1Picker/_out2Picker.

    auto buildGroup = [&](int startRow,
                          const std::vector<std::string>& outRows,
                          const std::string& groupId,
                          const std::string& groupLabel,
                          bool buttonsDisabled,
                          QComboBox** outPickerPtr) -> int
    {
        const int headerGroupRow   = startRow;
        const int headerChannelRow = startRow + 1;
        const int arrowRow         = startRow + 2;
        const int dataStartRow     = startRow + 3;

        // Row 0 of group: group header labels — "in 1", "const", "in 2", group name
        if (in1Count > 0)
        {
            QLabel* in1Header = new QLabel("in 1", this);
            in1Header->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(in1Header, headerGroupRow, 0, 1, in1Count);
        }
        // const column group header — spans both "0" and "1" columns
        {
            QLabel* constHeader = new QLabel("const", this);
            constHeader->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(constHeader, headerGroupRow, const0Col, 1, 2);
        }
        if (in2Count > 0)
        {
            QLabel* in2Header = new QLabel("in 2", this);
            in2Header->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(in2Header, headerGroupRow, in2StartCol, 1, in2Count);
        }
        // Output picker: a QComboBox placed in the outLabelCol at the group header row.
        // The picker spans all data rows of this group via rowspan so it stays
        // visually aligned with the button rows on the left.
        {
            const int dataRowCount = static_cast<int>(outRows.size());
            const int pickerRowSpan = 3 + (dataRowCount > 0 ? dataRowCount : 1);
            QComboBox* outPicker = makePicker(groupLabel,
                [this, groupId](const QString& selectedText)
                {
                    // Guard: widget might have been destroyed between timer post and fire.
                    if (!_knob) return;
                    const std::string layerText = selectedText.toStdString();
                    // Look up the ChannelSet knob by its NDK name ("out1" or "out2").
                    // set_text() stores the new layer name into the knob's value.
                    // changed() notifies the Nuke event system which then calls
                    // Op::knob_changed — this is the correct NDK trigger pattern.
                    DD::Image::Knob* targetKnob = _knob->op()->knob(groupId.c_str());
                    if (targetKnob)
                    {
                        targetKnob->set_text(layerText == "none" ? "none" : layerText.c_str());
                        targetKnob->changed();
                    }
                });
            outPicker->setEnabled(!buttonsDisabled);
            _gridLayout->addWidget(outPicker, headerGroupRow, outLabelCol, pickerRowSpan, 1);
            if (outPickerPtr)
                *outPickerPtr = outPicker;
        }

        // Row 1 of group: per-column short channel name labels
        for (int i = 0; i < in1Count; ++i)
        {
            QLabel* label = new QLabel(
                QString::fromStdString(shortChannelName(in1Columns[i])), this);
            label->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(label, headerChannelRow, i);
        }
        // const column channel labels: "0" and "1"
        {
            QLabel* const0Label = new QLabel("0", this);
            const0Label->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(const0Label, headerChannelRow, const0Col);
        }
        {
            QLabel* const1Label = new QLabel("1", this);
            const1Label->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(const1Label, headerChannelRow, const1Col);
        }
        for (int i = 0; i < in2Count; ++i)
        {
            QLabel* label = new QLabel(
                QString::fromStdString(shortChannelName(in2Columns[i])), this);
            label->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(label, headerChannelRow, in2StartCol + i);
        }

        // Row 2 of group: down-arrow labels under each source column
        // Use the double-headed arrow (⇓, U+21D3) for a wider, more visible glyph.
        const QString downArrow = QString::fromUtf8("\xe2\x87\x93");  // "⇓"
        for (int i = 0; i < in1Count; ++i)
            _gridLayout->addWidget(makeArrowLabel(downArrow), arrowRow, i);
        _gridLayout->addWidget(makeArrowLabel(downArrow), arrowRow, const0Col);
        _gridLayout->addWidget(makeArrowLabel(downArrow), arrowRow, const1Col);
        for (int i = 0; i < in2Count; ++i)
            _gridLayout->addWidget(makeArrowLabel(downArrow), arrowRow, in2StartCol + i);

        // Data rows: one per output channel in this group
        for (int rowIdx = 0; rowIdx < static_cast<int>(outRows.size()); ++rowIdx)
        {
            const std::string& outputName = outRows[rowIdx];
            const int gridRow = dataStartRow + rowIdx;

            // capturedGroupId scopes radio enforcement: "out1" or "out2".
            // This ensures rows across the two output groups remain independent
            // even when their channel names are identical (e.g. rgba.red in both).
            const std::string capturedGroupId = groupId;

            // in1 toggle buttons
            for (int si = 0; si < in1Count; ++si)
            {
                const std::string& sourceName = in1Columns[si];
                ChannelButton* btn = new ChannelButton(
                    nukeChannelColor(shortChannelName(sourceName)), this);
                // objectName: "groupId|outputChannel|inputGroup|sourceChannel"
                // groupId ("out1" or "out2") scopes radio enforcement so
                // out1 and out2 rows with matching channel names stay independent.
                btn->setObjectName(
                    QString::fromStdString(capturedGroupId + "|" + outputName + "|in1|" + sourceName));
                btn->setEnabled(!buttonsDisabled);
                const std::string capturedOutput = outputName;
                const std::string capturedSource = sourceName;
                connect(btn, &QPushButton::toggled,
                        [this, capturedGroupId, capturedOutput, capturedSource](bool checked)
                        { this->onCellToggled(capturedGroupId, capturedOutput, "in1", capturedSource, checked); });
                _gridLayout->addWidget(btn, gridRow, si);
                _toggleButtons.push_back(btn);
            }

            // const:0 toggle button — near-black color (represents constant 0.0 value)
            {
                ChannelButton* btn = new ChannelButton(QColor(30, 30, 30), this);
                btn->setObjectName(
                    QString::fromStdString(capturedGroupId + "|" + outputName + "|in1|const:0"));
                btn->setEnabled(!buttonsDisabled);
                const std::string capturedOutput = outputName;
                connect(btn, &QPushButton::toggled,
                        [this, capturedGroupId, capturedOutput](bool checked)
                        { this->onCellToggled(capturedGroupId, capturedOutput, "in1", "const:0", checked); });
                _gridLayout->addWidget(btn, gridRow, const0Col);
                _toggleButtons.push_back(btn);
            }

            // const:1 toggle button — near-white color (represents constant 1.0 value)
            {
                ChannelButton* btn = new ChannelButton(QColor(220, 220, 220), this);
                btn->setObjectName(
                    QString::fromStdString(capturedGroupId + "|" + outputName + "|in1|const:1"));
                btn->setEnabled(!buttonsDisabled);
                const std::string capturedOutput = outputName;
                connect(btn, &QPushButton::toggled,
                        [this, capturedGroupId, capturedOutput](bool checked)
                        { this->onCellToggled(capturedGroupId, capturedOutput, "in1", "const:1", checked); });
                _gridLayout->addWidget(btn, gridRow, const1Col);
                _toggleButtons.push_back(btn);
            }

            // in2 toggle buttons — disabled when in2=Chan_Black (placeholder columns)
            for (int si = 0; si < in2Count; ++si)
            {
                const std::string& sourceName = in2Columns[si];
                ChannelButton* btn = new ChannelButton(
                    nukeChannelColor(shortChannelName(sourceName)), this);
                btn->setObjectName(
                    QString::fromStdString(capturedGroupId + "|" + outputName + "|in2|" + sourceName));
                btn->setEnabled(!in2Disabled);
                const std::string capturedOutput = outputName;
                const std::string capturedSource = sourceName;
                connect(btn, &QPushButton::toggled,
                        [this, capturedGroupId, capturedOutput, capturedSource](bool checked)
                        { this->onCellToggled(capturedGroupId, capturedOutput, "in2", capturedSource, checked); });
                _gridLayout->addWidget(btn, gridRow, in2StartCol + si);
                _toggleButtons.push_back(btn);
            }

            // Output channel name label on the right — right-arrow prefix for visual clarity.
            // Use ⇒ (U+21D2, double-headed right arrow) for a wider, more visible glyph.
            QLabel* outLabel = new QLabel(
                QString::fromUtf8("\xe2\x87\x92 ") +       // "⇒ "
                QString::fromStdString(shortChannelName(outputName)), this);
            outLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            outLabel->setEnabled(!buttonsDisabled);
            {
                QFont labelFont = outLabel->font();
                labelFont.setPointSize(labelFont.pointSize() + 2);
                outLabel->setFont(labelFont);
            }
            _gridLayout->addWidget(outLabel, gridRow, outLabelCol);
        }

        return dataStartRow + static_cast<int>(outRows.size());
    };

    // ---- Row 0: in1 and in2 ChannelSet pickers ----
    //
    // in1 picker spans the in1 toggle columns; in2 picker spans const + in2 columns.
    // Both are placed in the same row (row 0) so they sit directly above their
    // respective column groups, matching the target 2-column layout:
    //
    //   [in1 picker]           [in2 picker]
    //   [in1→out1 matrix]      [in2→out1 matrix]   [out1 picker]
    //   [in1→out2 matrix]      [in2→out2 matrix]   [out2 picker]

    const int pickerRow = 0;

    {
        const std::string in1LayerName = layerNameFromChannelSet(in1Set);
        _in1Picker = makePicker(
            in1LayerName.empty() ? "none" : in1LayerName,
            [this](const QString& selectedText)
            {
                if (!_knob) return;
                DD::Image::Knob* targetKnob = _knob->op()->knob("in1");
                if (targetKnob)
                {
                    targetKnob->set_text(selectedText.toStdString().c_str());
                    targetKnob->changed();
                }
            });
        // Span in1 columns; fall back to 1 column if in1 is empty.
        const int in1Span = (in1Count > 0) ? in1Count : 1;
        _gridLayout->addWidget(_in1Picker, pickerRow, 0, 1, in1Span);
    }

    {
        const std::string in2LayerName = layerNameFromChannelSet(in2Set);
        _in2Picker = makePicker(
            (in2Disabled || in2LayerName.empty()) ? "none" : in2LayerName,
            [this](const QString& selectedText)
            {
                if (!_knob) return;
                DD::Image::Knob* targetKnob = _knob->op()->knob("in2");
                if (targetKnob)
                {
                    targetKnob->set_text(selectedText.toStdString().c_str());
                    targetKnob->changed();
                }
            });
        // Span const columns (2) + in2 columns. If in2 has placeholder cols,
        // in2Count is already 4 (placeholders), so this will span correctly.
        const int in2Span = 2 + in2Count;
        _gridLayout->addWidget(_in2Picker, pickerRow, const0Col, 1, in2Span);
    }

    // Groups start at row 1 (row 0 is occupied by the input pickers).
    int nextRow = 1;

    // ---- Build out1 group ----
    // groupId "out1" is a stable identity token used in button objectNames for
    // radio-scope enforcement. groupLabel is the human-readable layer name shown
    // in the panel UI. These two values must be kept independent.
    const std::string out1Label = layerNameFromChannelSet(out1Set);
    nextRow = buildGroup(nextRow, out1Rows, "out1",
                         out1Label.empty() ? "none" : out1Label,
                         false, &_out1Picker);

    // ---- Build out2 group (with spacer above) ----
    // Always render the out2 group — when out2 is none (Chan_Black) the group is
    // shown but all buttons are disabled so the layout does not collapse.
    if (!out2Rows.empty())
    {
        _gridLayout->setRowMinimumHeight(nextRow, 10);
        ++nextRow;

        const std::string out2Label = layerNameFromChannelSet(out2Set);
        nextRow = buildGroup(nextRow, out2Rows, "out2",
                             (out2Disabled || out2Label.empty()) ? "none" : out2Label,
                             out2Disabled, &_out2Picker);
    }

    // Ensure Nuke allocates enough vertical space in the panel.
    // Use 28px per row to account for the extra arrow row added per group.
    setMinimumHeight((nextRow + 1) * 28);
}

void ShuffleMatrixWidget::clearLayout()
{
    // Null picker pointers before deleting — they will be deleted by the layout
    // takeAt loop below as child widgets. Nulling here prevents any dangling
    // dereference if a deferred QTimer fires between clearLayout and buildLayout.
    _in1Picker  = nullptr;
    _in2Picker  = nullptr;
    _out1Picker = nullptr;
    _out2Picker = nullptr;

    while (QLayoutItem* item = _gridLayout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    _toggleButtons.clear();

    // Recreate the layout to reset column/row minimum sizes set by buildLayout().
    delete _gridLayout;
    _gridLayout = new QGridLayout(this);
    _gridLayout->setHorizontalSpacing(2);
    _gridLayout->setVerticalSpacing(2);
    _gridLayout->setContentsMargins(4, 4, 4, 4);
    setLayout(_gridLayout);
}

// ---------------------------------------------------------------------------
// Sync from knob state
// ---------------------------------------------------------------------------

void ShuffleMatrixWidget::syncFromKnob()
{
    if (!_knob)
        return;

    // Identity routing initialisation — runs once on first panel open when no
    // state has been serialized yet. Uses initializeState() to write directly
    // without calling changed(), avoiding a recursive rebuild.
    if (_knob->matrixState().empty())
    {
        const DD::Image::ChannelSet& in1Set  = _knob->in1ChannelSet();
        const DD::Image::ChannelSet& out1Set = _knob->out1ChannelSet();

        if (in1Set  != DD::Image::ChannelSet(DD::Image::Chan_Black) &&
            out1Set != DD::Image::ChannelSet(DD::Image::Chan_Black))
        {
            std::ostringstream identityStream;
            bool firstEntry = true;
            DD::Image::Channel outChannel;
            foreach (outChannel, out1Set)
            {
                const std::string outName = DD::Image::getName(outChannel);
                DD::Image::Channel inChannel;
                foreach (inChannel, in1Set)
                {
                    const std::string inName = DD::Image::getName(inChannel);
                    if (outName == inName)
                    {
                        if (!firstEntry) identityStream << ',';
                        identityStream << outName << ":in1:" << inName;
                        firstEntry = false;
                        break;
                    }
                }
            }
            const std::string identityState = identityStream.str();
            if (!identityState.empty())
                _knob->initializeState(identityState);
        }
    }

    clearLayout();
    buildLayout();

    // Parse routing state.
    // New format:  "outName:in1:srcName,outName:in2:srcName,..."
    // Legacy format (no group tag): "outName:srcName,..."  treated as "in1".
    //
    // routingMap key:   outputChannelName
    // routingMap value: "groupId:srcName"  (e.g. "in1:rgba.red")
    std::map<std::string, std::string> routingMap;
    {
        const std::string& stateString = _knob->matrixState();
        std::istringstream stateStream(stateString);
        std::string token;
        while (std::getline(stateStream, token, ','))
        {
            if (token.empty()) continue;
            const std::string::size_type firstColon = token.find(':');
            if (firstColon == std::string::npos) continue;

            const std::string outName = token.substr(0, firstColon);
            const std::string rest    = token.substr(firstColon + 1);

            // Determine if rest starts with a group tag ("in1:" or "in2:").
            std::string groupedValue;
            if (rest.size() > 4 &&
                (rest.substr(0, 4) == "in1:" || rest.substr(0, 4) == "in2:"))
            {
                groupedValue = rest;  // already "groupId:srcName"
            }
            else
            {
                // Legacy format — assume in1.
                groupedValue = "in1:" + rest;
            }

            routingMap[outName] = groupedValue;
        }
    }

    // Set each button's checked state.
    // objectName format: "outGroup|outputChannel|inputGroup|sourceChannel"
    for (ChannelButton* btn : _toggleButtons)
    {
        const QString qName   = btn->objectName();
        const int firstPipe   = qName.indexOf('|');
        const int secondPipe  = qName.indexOf('|', firstPipe + 1);
        const int thirdPipe   = qName.indexOf('|', secondPipe + 1);
        if (firstPipe < 0 || secondPipe < 0 || thirdPipe < 0) continue;

        // outGroupId is parsed but not used for routing lookup (routing map is keyed
        // by outputName only); it exists for radio enforcement in onCellToggled.
        const std::string outputName = qName.mid(firstPipe + 1, secondPipe - firstPipe - 1).toStdString();
        const std::string groupId    = qName.mid(secondPipe + 1, thirdPipe - secondPipe - 1).toStdString();
        const std::string sourceName = qName.mid(thirdPipe + 1).toStdString();

        const QSignalBlocker blocker(btn);
        auto it = routingMap.find(outputName);
        if (it != routingMap.end())
        {
            // routingMap value is "groupId:srcName"; match both.
            const std::string expected = groupId + ":" + sourceName;
            btn->setChecked(it->second == expected);
        }
        else
        {
            btn->setChecked(false);
        }
    }
}

// ---------------------------------------------------------------------------
// User interaction slot
// ---------------------------------------------------------------------------

void ShuffleMatrixWidget::onCellToggled(const std::string& outputGroup,
                                         const std::string& outputChannelName,
                                         const std::string& inputGroup,
                                         const std::string& sourceColumnName,
                                         bool checked)
{
    if (!_knob)
        return;
    if (!checked) return;  // no-op: rows must never be left with no source selected

    // Parse current routing state into a mutable map.
    // Map value format: "groupId:srcName"
    std::map<std::string, std::string> routingMap;
    {
        const std::string& stateString = _knob->matrixState();
        std::istringstream stateStream(stateString);
        std::string token;
        while (std::getline(stateStream, token, ','))
        {
            if (token.empty()) continue;
            const std::string::size_type firstColon = token.find(':');
            if (firstColon == std::string::npos) continue;

            const std::string outName = token.substr(0, firstColon);
            const std::string rest    = token.substr(firstColon + 1);

            if (rest.size() > 4 &&
                (rest.substr(0, 4) == "in1:" || rest.substr(0, 4) == "in2:"))
            {
                routingMap[outName] = rest;
            }
            else
            {
                routingMap[outName] = "in1:" + rest;
            }
        }
    }

    const std::string newValue = inputGroup + ":" + sourceColumnName;

    // Radio-button enforcement: only one source per output channel within the same
    // output group. The outputGroup scope prevents out1 and out2 rows from sharing
    // radio state when they have identically-named channels.
    for (ChannelButton* otherBtn : _toggleButtons)
    {
        const QString qName       = otherBtn->objectName();
        const int firstPipe       = qName.indexOf('|');
        const int secondPipe      = qName.indexOf('|', firstPipe + 1);
        const int thirdPipe       = qName.indexOf('|', secondPipe + 1);
        if (firstPipe < 0 || secondPipe < 0 || thirdPipe < 0) continue;

        const std::string otherOutputGroup = qName.left(firstPipe).toStdString();
        const std::string otherOutput      = qName.mid(firstPipe + 1, secondPipe - firstPipe - 1).toStdString();
        const std::string otherGroup       = qName.mid(secondPipe + 1, thirdPipe - secondPipe - 1).toStdString();
        const std::string otherSource      = qName.mid(thirdPipe + 1).toStdString();

        // Same output row AND same output group; different button than the one just checked.
        if (otherOutput == outputChannelName &&
            otherOutputGroup == outputGroup &&
            !(otherGroup == inputGroup && otherSource == sourceColumnName))
        {
            const QSignalBlocker blocker(otherBtn);
            otherBtn->setChecked(false);
        }
    }

    // Erase any previous routing for this output, then store the new one.
    routingMap.erase(outputChannelName);
    routingMap[outputChannelName] = newValue;

    // Serialize back to comma-separated string.
    // Format: "outName:groupId:srcName,..."
    std::ostringstream out;
    bool first = true;
    for (const auto& entry : routingMap)
    {
        if (!first) out << ',';
        out << entry.first << ':' << entry.second;
        first = false;
    }

    _knob->setValue(out.str());
}
