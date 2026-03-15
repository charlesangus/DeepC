#include "ShuffleMatrixWidget.h"
#include "ShuffleMatrixKnob.h"
#include "DDImage/Channel.h"
#include "DDImage/ChannelSet.h"
#include <QTabWidget>
#include <sstream>
#include <map>
#include <algorithm>

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
// Layout construction helpers
// ---------------------------------------------------------------------------

void ShuffleMatrixWidget::buildLayout()
{
    // Collect output channel names (rows) — up to 8 from in1ChannelSet.
    _outputChannelNames.clear();
    {
        const DD::Image::ChannelSet& in1Set = _knob->in1ChannelSet();
        DD::Image::Channel channelIterator;
        foreach(channelIterator, in1Set)
        {
            if (_outputChannelNames.size() >= 8)
                break;
            _outputChannelNames.push_back(DD::Image::getName(channelIterator));
        }
    }

    // Collect source channel names (columns) — in1 first, then in2 if not Chan_Black.
    _sourceChannelNames.clear();
    {
        const DD::Image::ChannelSet& in1Set = _knob->in1ChannelSet();
        DD::Image::Channel channelIterator;
        foreach(channelIterator, in1Set)
        {
            _sourceChannelNames.push_back(DD::Image::getName(channelIterator));
        }
    }
    {
        const DD::Image::ChannelSet& in2Set = _knob->in2ChannelSet();
        if (in2Set != DD::Image::ChannelSet(DD::Image::Chan_Black))
        {
            DD::Image::Channel channelIterator;
            foreach(channelIterator, in2Set)
            {
                _sourceChannelNames.push_back(DD::Image::getName(channelIterator));
            }
        }
    }

    // Row 0: header — empty cell at (0,0), source channel name labels at (0, col+1).
    _gridLayout->addWidget(new QLabel("", this), 0, 0);
    for (int sourceIndex = 0; sourceIndex < static_cast<int>(_sourceChannelNames.size()); ++sourceIndex)
    {
        QLabel* headerLabel = new QLabel(QString::fromStdString(_sourceChannelNames[sourceIndex]), this);
        headerLabel->setAlignment(Qt::AlignCenter);
        // Rotate header labels 90 degrees so they fit in narrow columns.
        headerLabel->setWordWrap(false);
        _gridLayout->addWidget(headerLabel, 0, sourceIndex + 1);
    }

    // Rows 1..N: output channel label at (row, 0), toggle buttons at (row, sourceIndex+1).
    _toggleButtons.clear();
    for (int outputIndex = 0; outputIndex < static_cast<int>(_outputChannelNames.size()); ++outputIndex)
    {
        const std::string& outputName = _outputChannelNames[outputIndex];
        int gridRow = outputIndex + 1;

        QLabel* rowLabel = new QLabel(QString::fromStdString(outputName), this);
        rowLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
        _gridLayout->addWidget(rowLabel, gridRow, 0);

        for (int sourceIndex = 0; sourceIndex < static_cast<int>(_sourceChannelNames.size()); ++sourceIndex)
        {
            const std::string& sourceName = _sourceChannelNames[sourceIndex];

            QPushButton* toggleButton = new QPushButton(this);
            toggleButton->setCheckable(true);
            toggleButton->setFixedHeight(22);
            toggleButton->setFixedWidth(22);

            // Store identities in object name: "outputName|sourceName"
            toggleButton->setObjectName(QString::fromStdString(outputName + "|" + sourceName));

            // Capture by value for use in lambda.
            const std::string capturedOutputName = outputName;
            const std::string capturedSourceName = sourceName;
            connect(toggleButton, &QPushButton::toggled,
                    [this, capturedOutputName, capturedSourceName](bool checked)
                    {
                        this->onCellToggled(capturedOutputName, capturedSourceName, checked);
                    });

            _gridLayout->addWidget(toggleButton, gridRow, sourceIndex + 1);
            _toggleButtons.push_back(toggleButton);
        }
    }

    // Provide a sensible minimum height so Nuke's panel allocates enough space.
    setMinimumHeight(static_cast<int>(_outputChannelNames.size() + 1) * 28);
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
    _sourceChannelNames.clear();
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
    std::map<std::string, std::string> routingMap; // outputChannelName -> sourceChannelName
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
            const std::string sourceChannelName  = token.substr(colonPosition + 1);
            routingMap[outputChannelName] = sourceChannelName;
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
        const std::string sourceChannelName  = objectName.mid(separatorPosition + 1).toStdString();

        // Block signals to prevent onCellToggled from firing during sync.
        const QSignalBlocker signalBlocker(toggleButton);
        auto mapIterator = routingMap.find(outputChannelName);
        const bool shouldBeChecked = (mapIterator != routingMap.end())
                                     && (mapIterator->second == sourceChannelName);
        toggleButton->setChecked(shouldBeChecked);
    }
}

// ---------------------------------------------------------------------------
// User interaction slot
// ---------------------------------------------------------------------------

void ShuffleMatrixWidget::onCellToggled(const std::string& outputChannelName,
                                         const std::string& sourceChannelName,
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
        // Route this source channel to the output channel.
        routingMap[outputChannelName] = sourceChannelName;

        // Enforce single-source-per-output constraint: uncheck all other buttons in
        // the same row (same output channel) so only one source button stays active.
        for (QPushButton* otherButton : _toggleButtons)
        {
            const QString otherName = otherButton->objectName();
            const int separatorPosition = otherName.indexOf('|');
            if (separatorPosition < 0)
                continue;
            const std::string otherOutputName = otherName.left(separatorPosition).toStdString();
            const std::string otherSourceName  = otherName.mid(separatorPosition + 1).toStdString();

            // Same row, different column: visually uncheck and remove from map.
            if (otherOutputName == outputChannelName && otherSourceName != sourceChannelName)
            {
                const QSignalBlocker signalBlocker(otherButton);
                otherButton->setChecked(false);
                routingMap.erase(otherOutputName);
            }
        }
        // Restore the current cell's mapping (it may have been erased above if
        // another entry with the same output key was processed in the loop).
        routingMap[outputChannelName] = sourceChannelName;
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
