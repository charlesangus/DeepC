#include "ShuffleMatrixWidget.h"
#include "ShuffleMatrixKnob.h"
#include "DDImage/Channel.h"
#include "DDImage/ChannelSet.h"
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

    const bool in2Active = (in2Set != DD::Image::ChannelSet(DD::Image::Chan_Black));
    if (in2Active)
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

    const bool out2Active = (out2Set != DD::Image::ChannelSet(DD::Image::Chan_Black));
    if (out2Active)
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
    //   Col  in1Count                  : visual spacer between in1 and in2
    //   Cols in1Count+1 .. +in2Count   : in2 toggle columns (if in2 active)
    //   Col  outLabelCol               : output channel name / group name label

    const int in1Count    = static_cast<int>(in1Columns.size());
    const int in2Count    = static_cast<int>(in2Columns.size());
    const int spacerCol   = in1Count;
    const int in2StartCol = in2Active ? in1Count + 1 : in1Count;
    const int outLabelCol = in2StartCol + in2Count;

    if (in2Active && in2Count > 0)
        _gridLayout->setColumnMinimumWidth(spacerCol, 12);

    _toggleButtons.clear();

    // ---- Lambda: build one output group (two header rows + data rows) ----
    //
    // Each group gets its own "in 1" / "in 2" header and per-column channel
    // labels so it reads as a self-contained matrix, not part of a shared block.
    // Returns the next unused grid row.

    auto buildGroup = [&](int startRow,
                          const std::vector<std::string>& outRows,
                          const std::string& groupLabel) -> int
    {
        const int headerGroupRow   = startRow;
        const int headerChannelRow = startRow + 1;
        const int dataStartRow     = startRow + 2;

        // Row 0 of group: "in 1" / "in 2" span labels + group name on right
        if (in1Count > 0)
        {
            QLabel* in1Header = new QLabel("in 1", this);
            in1Header->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(in1Header, headerGroupRow, 0, 1, in1Count);
        }
        if (in2Active && in2Count > 0)
        {
            QLabel* in2Header = new QLabel("in 2", this);
            in2Header->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(in2Header, headerGroupRow, in2StartCol, 1, in2Count);
        }
        if (!groupLabel.empty())
        {
            QLabel* groupNameLabel = new QLabel(QString::fromStdString(groupLabel), this);
            groupNameLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            _gridLayout->addWidget(groupNameLabel, headerGroupRow, outLabelCol);
        }

        // Row 1 of group: per-column short channel name labels
        for (int i = 0; i < in1Count; ++i)
        {
            QLabel* label = new QLabel(
                QString::fromStdString(shortChannelName(in1Columns[i])), this);
            label->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(label, headerChannelRow, i);
        }
        for (int i = 0; i < in2Count; ++i)
        {
            QLabel* label = new QLabel(
                QString::fromStdString(shortChannelName(in2Columns[i])), this);
            label->setAlignment(Qt::AlignCenter);
            _gridLayout->addWidget(label, headerChannelRow, in2StartCol + i);
        }

        // Data rows: one per output channel in this group
        for (int rowIdx = 0; rowIdx < static_cast<int>(outRows.size()); ++rowIdx)
        {
            const std::string& outputName = outRows[rowIdx];
            const int gridRow = dataStartRow + rowIdx;

            const std::string capturedOutGroup = groupLabel;

            // in1 toggle buttons
            for (int si = 0; si < in1Count; ++si)
            {
                const std::string& sourceName = in1Columns[si];
                QPushButton* btn = new QPushButton(this);
                btn->setCheckable(true);
                btn->setFixedHeight(22);
                btn->setFixedWidth(22);
                // objectName: "outGroup|outputChannel|in1|sourceChannel"
                // Four-field format: outGroup prefix scopes radio enforcement so
                // out1 and out2 rows with matching channel names stay independent.
                btn->setObjectName(
                    QString::fromStdString(capturedOutGroup + "|" + outputName + "|in1|" + sourceName));
                const std::string capturedOutput = outputName;
                const std::string capturedSource = sourceName;
                connect(btn, &QPushButton::toggled,
                        [this, capturedOutGroup, capturedOutput, capturedSource](bool checked)
                        { this->onCellToggled(capturedOutGroup, capturedOutput, "in1", capturedSource, checked); });
                _gridLayout->addWidget(btn, gridRow, si);
                _toggleButtons.push_back(btn);
            }

            // in2 toggle buttons
            for (int si = 0; si < in2Count; ++si)
            {
                const std::string& sourceName = in2Columns[si];
                QPushButton* btn = new QPushButton(this);
                btn->setCheckable(true);
                btn->setFixedHeight(22);
                btn->setFixedWidth(22);
                btn->setObjectName(
                    QString::fromStdString(capturedOutGroup + "|" + outputName + "|in2|" + sourceName));
                const std::string capturedOutput = outputName;
                const std::string capturedSource = sourceName;
                connect(btn, &QPushButton::toggled,
                        [this, capturedOutGroup, capturedOutput, capturedSource](bool checked)
                        { this->onCellToggled(capturedOutGroup, capturedOutput, "in2", capturedSource, checked); });
                _gridLayout->addWidget(btn, gridRow, in2StartCol + si);
                _toggleButtons.push_back(btn);
            }

            // Output channel name label on the right
            QLabel* outLabel = new QLabel(
                QString::fromStdString(shortChannelName(outputName)), this);
            outLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            _gridLayout->addWidget(outLabel, gridRow, outLabelCol);
        }

        return dataStartRow + static_cast<int>(outRows.size());
    };

    // ---- Build out1 group ----
    const std::string out1Label = layerNameFromChannelSet(out1Set);
    int nextRow = buildGroup(0, out1Rows, out1Label.empty() ? "out 1" : out1Label);

    // ---- Build out2 group (with spacer above) ----
    if (out2Active && !out2Rows.empty())
    {
        _gridLayout->setRowMinimumHeight(nextRow, 10);
        ++nextRow;

        const std::string out2Label = layerNameFromChannelSet(out2Set);
        nextRow = buildGroup(nextRow, out2Rows, out2Label.empty() ? "out 2" : out2Label);
    }

    // Ensure Nuke allocates enough vertical space in the panel.
    setMinimumHeight(nextRow * 26);
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
    for (QPushButton* btn : _toggleButtons)
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
    for (QPushButton* otherBtn : _toggleButtons)
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
