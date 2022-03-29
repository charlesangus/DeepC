#pragma once

#include "DDImage/DeepOp.h"

struct DeepCOpTreeSpec
{
    int _dimensions;
    int _blurMode;
};

struct DeepCOpTree
{
private:
    std::vector<DD::Image::Op*> _opTree;
    DD::Image::DeepInfo _deepInfo;
public:
    ~DeepCOpTree();

    bool set_input(DD::Image::Op*);
    bool set_parent(DD::Image::Op* op);
    void push_back(DD::Image::Op* op);
    void open();
    DD::Image::Op* back();

    bool validate(bool for_real);
    const DD::Image::DeepInfo& deepInfo();
    template<typename... Params>
    bool deepEngine(DD::Image::Box box, const DD::Image::ChannelSet& channels, DD::Image::DeepPlane& plane)
    {
        if (_opTree.empty())
        {
            return false;
        }
        DD::Image::DeepOnlyOp* lastOp = dynamic_cast<DeepOnlyOp*>(_opTree.back());
        if (lastOp == nullptr)
        {
            return false;
        }
        return lastOp->deepEngine(box,channels,plane);
    }

    bool empty();
    bool isValid();
};
