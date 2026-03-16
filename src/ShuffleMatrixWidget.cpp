#include "ShuffleMatrixWidget.h"
#include "ShuffleMatrixKnob.h"
#include "DDImage/Channel.h"
#include "DDImage/ChannelSet.h"
#include "DDImage/Op.h"
#include <QTabWidget>
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
        painter.setPen(QPen(Qt::white, 2));
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
    _gridLayout->setSpacing(2);
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
    const bool in2Disabled = (in2Set == DD::Image::ChannelSet(DD::Image::Chan_Black));
    {
        DD::Image::Channel ch;
        foreach(ch, in2Set)
            in2Columns.push_back(DD::Image::getName(ch));
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

    // ---- Lambda: build one output group (two header rows + data rows) ----
    //
    // Each group gets its own "in 1" / "in 2" header and per-column channel
    // labels so it reads as a self-contained matrix, not part of a shared block.
    // Returns the next unused grid row.
    //
    // groupId  — stable identity string used in button objectNames for radio
    //            enforcement ("out1" or "out2"). Must be unique across groups.
    // groupLabel — human-readable display name shown in the panel (layer name
    //              or "out 1"/"out 2" fallback). Independent of groupId.

    auto buildGroup = [&](int startRow,
                          const std::vector<std::string>& outRows,
                          const std::string& groupId,
                          const std::string& groupLabel,
                          bool buttonsDisabled) -> int
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
        if (!groupLabel.empty())
        {
            QLabel* groupNameLabel = new QLabel(QString::fromStdString(groupLabel), this);
            groupNameLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            _gridLayout->addWidget(groupNameLabel, headerGroupRow, outLabelCol, 1, 1);
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
        for (int i = 0; i < in1Count; ++i)
        {
            QLabel* arrowLabel = new QLabel(QString::fromUtf8("\xe2\x86\x93"), this);  // "↓"
            arrowLabel->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(arrowLabel, arrowRow, i);
        }
        {
            QLabel* arrowLabel = new QLabel(QString::fromUtf8("\xe2\x86\x93"), this);
            arrowLabel->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(arrowLabel, arrowRow, const0Col);
        }
        {
            QLabel* arrowLabel = new QLabel(QString::fromUtf8("\xe2\x86\x93"), this);
            arrowLabel->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(arrowLabel, arrowRow, const1Col);
        }
        for (int i = 0; i < in2Count; ++i)
        {
            QLabel* arrowLabel = new QLabel(QString::fromUtf8("\xe2\x86\x93"), this);
            arrowLabel->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(arrowLabel, arrowRow, in2StartCol + i);
        }

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

            // Output channel name label on the right — right-arrow prefix for visual clarity
            QLabel* outLabel = new QLabel(
                QString::fromUtf8("\xe2\x86\x92 ") +       // "→ "
                QString::fromStdString(shortChannelName(outputName)), this);
            outLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            outLabel->setEnabled(!buttonsDisabled);
            _gridLayout->addWidget(outLabel, gridRow, outLabelCol);
        }

        return dataStartRow + static_cast<int>(outRows.size());
    };

    // ---- Build out1 group ----
    // groupId "out1" is a stable identity token used in button objectNames for
    // radio-scope enforcement. groupLabel is the human-readable layer name shown
    // in the panel UI. These two values must be kept independent.
    const std::string out1Label = layerNameFromChannelSet(out1Set);
    int nextRow = buildGroup(0, out1Rows, "out1", out1Label.empty() ? "out 1" : out1Label, false);

    // ---- Build out2 group (with spacer above) ----
    // Always render the out2 group — when out2 is none (Chan_Black) the group is
    // shown but all buttons are disabled so the layout does not collapse.
    if (!out2Rows.empty())
    {
        _gridLayout->setRowMinimumHeight(nextRow, 10);
        ++nextRow;

        const std::string out2Label = layerNameFromChannelSet(out2Set);
        nextRow = buildGroup(nextRow, out2Rows, "out2", out2Label.empty() ? "out 2" : out2Label, out2Disabled);
    }

    // Ensure Nuke allocates enough vertical space in the panel.
    // Use 28px per row to account for the extra arrow row added per group.
    setMinimumHeight(nextRow * 28);
}

void ShuffleMatrixWidget::clearLayout()
{
    while (QLayoutItem* item = _gridLayout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    _toggleButtons.clear();

    // Recreate the layout to reset column/row minimum sizes set by buildLayout().
    delete _gridLayout;
    _gridLayout = new QGridLayout(this);
    _gridLayout->setSpacing(2);
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
