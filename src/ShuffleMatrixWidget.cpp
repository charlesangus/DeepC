#include "ShuffleMatrixWidget.h"
#include "ShuffleMatrixKnob.h"
#include "DDImage/Channel.h"
#include "DDImage/ChannelSet.h"
#include <QTabWidget>
#include <sstream>
#include <map>
#include <algorithm>

// Short display names extracted from a full NDK channel name like "rgba.red".
// Returns just the part after the last dot, or the full string if no dot found.
static std::string shortChannelName(const std::string& fullName)
{
    const std::string::size_type dotPosition = fullName.rfind('.');
    if (dotPosition != std::string::npos)
        return fullName.substr(dotPosition + 1);
    return fullName;
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
            // Walk up the parent chain; if inside a QTabWidget, use tab-aware visibility.
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
            // Null the knob pointer to prevent use-after-free.
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
    // -----------------------------------------------------------------------
    // Build source column list:
    //   in1 channels | const:0 | const:1 | in2 channels (if in2 active)
    // -----------------------------------------------------------------------
    _sourceColumnNames.clear();

    const DD::Image::ChannelSet& in1Set = _knob->in1ChannelSet();
    int in1ColumnCount = 0;
    {
        DD::Image::Channel channelIterator;
        foreach(channelIterator, in1Set)
        {
            _sourceColumnNames.push_back(DD::Image::getName(channelIterator));
            ++in1ColumnCount;
        }
    }

    // Always add constant 0 and 1 columns between the two input groups.
    _sourceColumnNames.push_back("const:0");
    _sourceColumnNames.push_back("const:1");

    const DD::Image::ChannelSet& in2Set = _knob->in2ChannelSet();
    int in2ColumnCount = 0;
    const bool in2Active = (in2Set != DD::Image::ChannelSet(DD::Image::Chan_Black));
    if (in2Active)
    {
        DD::Image::Channel channelIterator;
        foreach(channelIterator, in2Set)
        {
            _sourceColumnNames.push_back(DD::Image::getName(channelIterator));
            ++in2ColumnCount;
        }
    }

    const int totalSourceColumns = static_cast<int>(_sourceColumnNames.size());
    // Column 0 in the grid is reserved for the output group label (rgba =, none =).
    // Columns 1..totalSourceColumns are the source toggle columns.
    // Column totalSourceColumns+1 is the output channel name label on the right.
    const int firstSourceGridColumn  = 1;
    const int outputLabelGridColumn  = totalSourceColumns + 1;

    // -----------------------------------------------------------------------
    // Build output row list:
    //   out1 channels first, then out2 channels (if out2 active)
    // -----------------------------------------------------------------------
    _outputChannelNames.clear();

    const DD::Image::ChannelSet& out1Set = _knob->out1ChannelSet();
    int out1RowCount = 0;
    {
        DD::Image::Channel channelIterator;
        foreach(channelIterator, out1Set)
        {
            if (_outputChannelNames.size() >= 8)
                break;
            _outputChannelNames.push_back(DD::Image::getName(channelIterator));
            ++out1RowCount;
        }
    }

    const DD::Image::ChannelSet& out2Set = _knob->out2ChannelSet();
    int out2RowCount = 0;
    const bool out2Active = (out2Set != DD::Image::ChannelSet(DD::Image::Chan_Black));
    if (out2Active)
    {
        DD::Image::Channel channelIterator;
        foreach(channelIterator, out2Set)
        {
            if (_outputChannelNames.size() >= 8)
                break;
            _outputChannelNames.push_back(DD::Image::getName(channelIterator));
            ++out2RowCount;
        }
    }

    // -----------------------------------------------------------------------
    // Grid rows:
    //   0 — in-group header labels ("in 1", "0", "1", "in 2")
    //   1 — per-column channel name labels (r, g, b, a, 0, 1, r, g, b, a)
    //   2 — down-arrow labels
    //   3+ — output rows (one per output channel)
    // -----------------------------------------------------------------------

    // Row 0: in-group headers
    // Col 0: blank (reserved for output-group label)
    _gridLayout->addWidget(new QLabel("", this), 0, 0);

    if (in1ColumnCount > 0)
    {
        QLabel* in1Label = new QLabel("in 1", this);
        in1Label->setAlignment(Qt::AlignCenter);
        // Span all in1 source columns.
        _gridLayout->addWidget(in1Label, 0, firstSourceGridColumn, 1, in1ColumnCount);
    }

    // The "0" and "1" constant column headers (always present).
    // They sit at columns firstSourceGridColumn+in1ColumnCount and +in1ColumnCount+1.
    {
        QLabel* const0Header = new QLabel("", this);
        const0Header->setAlignment(Qt::AlignCenter);
        _gridLayout->addWidget(const0Header, 0, firstSourceGridColumn + in1ColumnCount);

        QLabel* const1Header = new QLabel("", this);
        const1Header->setAlignment(Qt::AlignCenter);
        _gridLayout->addWidget(const1Header, 0, firstSourceGridColumn + in1ColumnCount + 1);
    }

    if (in2Active && in2ColumnCount > 0)
    {
        QLabel* in2Label = new QLabel("in 2", this);
        in2Label->setAlignment(Qt::AlignCenter);
        // Span all in2 source columns.
        const int in2StartCol = firstSourceGridColumn + in1ColumnCount + 2; // skip 0 and 1
        _gridLayout->addWidget(in2Label, 0, in2StartCol, 1, in2ColumnCount);
    }

    // Col outputLabelGridColumn: blank in header rows
    _gridLayout->addWidget(new QLabel("", this), 0, outputLabelGridColumn);

    // Row 1: per-column short channel name labels
    _gridLayout->addWidget(new QLabel("", this), 1, 0);
    for (int sourceIndex = 0; sourceIndex < totalSourceColumns; ++sourceIndex)
    {
        const std::string& sourceName = _sourceColumnNames[sourceIndex];
        QString labelText;
        if (sourceName == "const:0")
            labelText = "0";
        else if (sourceName == "const:1")
            labelText = "1";
        else
            labelText = QString::fromStdString(shortChannelName(sourceName));

        QLabel* channelNameLabel = new QLabel(labelText, this);
        channelNameLabel->setAlignment(Qt::AlignCenter);
        _gridLayout->addWidget(channelNameLabel, 1, firstSourceGridColumn + sourceIndex);
    }
    _gridLayout->addWidget(new QLabel("", this), 1, outputLabelGridColumn);

    // Row 2: down-arrow labels under each source column
    _gridLayout->addWidget(new QLabel("", this), 2, 0);
    for (int sourceIndex = 0; sourceIndex < totalSourceColumns; ++sourceIndex)
    {
        QLabel* arrowLabel = new QLabel("\342\206\223", this); // UTF-8 for U+2193 DOWNWARDS ARROW
        arrowLabel->setAlignment(Qt::AlignCenter);
        _gridLayout->addWidget(arrowLabel, 2, firstSourceGridColumn + sourceIndex);
    }
    _gridLayout->addWidget(new QLabel("", this), 2, outputLabelGridColumn);

    // Rows 3+: output channel rows
    _toggleButtons.clear();
    for (int outputIndex = 0; outputIndex < static_cast<int>(_outputChannelNames.size()); ++outputIndex)
    {
        const std::string& outputName = _outputChannelNames[outputIndex];
        const int gridRow = 3 + outputIndex;

        // Col 0: output group label — show "rgba =" only on the first row of each group,
        // using a text label. The actual ChannelSet picker is a separate NDK knob in the
        // Op panel; this widget just provides a static text reminder.
        if (outputIndex == 0)
        {
            QLabel* out1GroupLabel = new QLabel("out1", this);
            out1GroupLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            _gridLayout->addWidget(out1GroupLabel, gridRow, 0);
        }
        else if (out2Active && outputIndex == out1RowCount)
        {
            QLabel* out2GroupLabel = new QLabel("out2", this);
            out2GroupLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            _gridLayout->addWidget(out2GroupLabel, gridRow, 0);
        }
        else
        {
            _gridLayout->addWidget(new QLabel("", this), gridRow, 0);
        }

        // Source toggle buttons — one per source column
        for (int sourceIndex = 0; sourceIndex < totalSourceColumns; ++sourceIndex)
        {
            const std::string& sourceName = _sourceColumnNames[sourceIndex];

            QPushButton* toggleButton = new QPushButton(this);
            toggleButton->setCheckable(true);
            toggleButton->setFixedHeight(22);
            toggleButton->setFixedWidth(22);

            // Object name encodes the routing pair for efficient state lookup.
            toggleButton->setObjectName(
                QString::fromStdString(outputName + "|" + sourceName));

            // Capture by value for use in lambda.
            const std::string capturedOutputName = outputName;
            const std::string capturedSourceName = sourceName;
            connect(toggleButton, &QPushButton::toggled,
                    [this, capturedOutputName, capturedSourceName](bool checked)
                    {
                        this->onCellToggled(capturedOutputName, capturedSourceName, checked);
                    });

            _gridLayout->addWidget(toggleButton,
                                   gridRow,
                                   firstSourceGridColumn + sourceIndex);
            _toggleButtons.push_back(toggleButton);
        }

        // Right-most column: short output channel name label (e.g. "red", "green")
        QLabel* outputNameLabel = new QLabel(
            QString::fromStdString(shortChannelName(outputName)), this);
        outputNameLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        _gridLayout->addWidget(outputNameLabel, gridRow, outputLabelGridColumn);
    }

    // Provide a sensible minimum height so Nuke's panel allocates enough space.
    // 3 header rows + N output rows, each ~28 px.
    const int totalRows = 3 + static_cast<int>(_outputChannelNames.size());
    setMinimumHeight(totalRows * 28);
}

void ShuffleMatrixWidget::clearLayout()
{
    // Remove and delete all child widgets from the grid.
    while (QLayoutItem* layoutItem = _gridLayout->takeAt(0))
    {
        delete layoutItem->widget();
        delete layoutItem;
    }
    _toggleButtons.clear();
    _outputChannelNames.clear();
    _sourceColumnNames.clear();
}

// ---------------------------------------------------------------------------
// Sync from knob state
// ---------------------------------------------------------------------------

void ShuffleMatrixWidget::syncFromKnob()
{
    if (!_knob)
        return;

    // Rebuild layout from the current ChannelSets stored in the knob.
    clearLayout();
    buildLayout();

    // Parse the current matrix state string into a lookup map.
    // Format: "outChanName:srcChanName,outChanName:srcChanName,..."
    std::map<std::string, std::string> routingMap; // outputChannelName -> sourceColumnName
    {
        const std::string& stateString = _knob->matrixState();
        std::istringstream stateStream(stateString);
        std::string token;
        while (std::getline(stateStream, token, ','))
        {
            if (token.empty())
                continue;
            const std::string::size_type colonPosition = token.find(':');
            if (colonPosition == std::string::npos)
                continue;
            const std::string outputChannelName = token.substr(0, colonPosition);
            const std::string sourceColumnName  = token.substr(colonPosition + 1);
            routingMap[outputChannelName] = sourceColumnName;
        }
    }

    // Sync each toggle button's checked state from the routing map.
    for (QPushButton* toggleButton : _toggleButtons)
    {
        const QString objectName = toggleButton->objectName();
        const int separatorPosition = objectName.indexOf('|');
        if (separatorPosition < 0)
            continue;
        const std::string outputChannelName = objectName.left(separatorPosition).toStdString();
        const std::string sourceColumnName  = objectName.mid(separatorPosition + 1).toStdString();

        // Block signals to prevent onCellToggled from firing during sync.
        const QSignalBlocker signalBlocker(toggleButton);
        auto mapIterator = routingMap.find(outputChannelName);
        const bool shouldBeChecked = (mapIterator != routingMap.end())
                                     && (mapIterator->second == sourceColumnName);
        toggleButton->setChecked(shouldBeChecked);
    }
}

// ---------------------------------------------------------------------------
// User interaction slot
// ---------------------------------------------------------------------------

void ShuffleMatrixWidget::onCellToggled(const std::string& outputChannelName,
                                         const std::string& sourceColumnName,
                                         bool checked)
{
    if (!_knob)
        return;

    // Parse the current routing state into a mutable map.
    std::map<std::string, std::string> routingMap;
    {
        const std::string& stateString = _knob->matrixState();
        std::istringstream stateStream(stateString);
        std::string token;
        while (std::getline(stateStream, token, ','))
        {
            if (token.empty())
                continue;
            const std::string::size_type colonPosition = token.find(':');
            if (colonPosition == std::string::npos)
                continue;
            routingMap[token.substr(0, colonPosition)] = token.substr(colonPosition + 1);
        }
    }

    if (checked)
    {
        // Route this source column to the output channel.
        routingMap[outputChannelName] = sourceColumnName;

        // Enforce single-source-per-output constraint: uncheck all other buttons in
        // the same row (same output channel) so only one source column stays active.
        for (QPushButton* otherButton : _toggleButtons)
        {
            const QString otherName = otherButton->objectName();
            const int separatorPosition = otherName.indexOf('|');
            if (separatorPosition < 0)
                continue;
            const std::string otherOutputName = otherName.left(separatorPosition).toStdString();
            const std::string otherSourceName  = otherName.mid(separatorPosition + 1).toStdString();

            // Same row, different column: visually uncheck and remove from map.
            if (otherOutputName == outputChannelName && otherSourceName != sourceColumnName)
            {
                const QSignalBlocker signalBlocker(otherButton);
                otherButton->setChecked(false);
                routingMap.erase(otherOutputName);
            }
        }
        // Restore the current cell's mapping (it may have been erased above if
        // another entry with the same output key was processed in the loop).
        routingMap[outputChannelName] = sourceColumnName;
    }
    else
    {
        // Unchecked: remove routing for this output (reverts to passthrough).
        routingMap.erase(outputChannelName);
    }

    // Serialize the updated routing map back to the comma-separated string.
    // Iterate in sorted key order for deterministic output.
    std::ostringstream serializedStream;
    bool firstEntry = true;
    for (const auto& routingEntry : routingMap)
    {
        if (!firstEntry)
            serializedStream << ',';
        serializedStream << routingEntry.first << ':' << routingEntry.second;
        firstEntry = false;
    }

    _knob->setValue(serializedStream.str());
}
