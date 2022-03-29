#include "DeepCOpTree.hpp"

using namespace DD::Image;

DeepCOpTree::~DeepCOpTree()
{
    const std::size_t opsAmount = _opTree.size();
    for (std::size_t opIndex=0; opIndex < opsAmount; ++opIndex)
    {
        //delete _opTree[opIndex];
    }
}

bool DeepCOpTree::set_input(Op* op)
{
    if (_opTree.empty())
    {
        return false;
    }
    _opTree.front()->set_input(0, op);
    return true;
}

bool DeepCOpTree::set_parent(Op* op)
{
    if (_opTree.empty())
    {
        return false;
    }
    _opTree.back()->parent(op);
    return true;
}

void DeepCOpTree::push_back(Op* op)
{
    _opTree.push_back(op);
    const std::size_t opsAmount = _opTree.size();
    if (opsAmount != 1)
    {
        _opTree[opsAmount - 1]->set_input(0, _opTree[opsAmount - 2]);
        _opTree[opsAmount - 2]->parent(_opTree[opsAmount - 1]);
    }
}

void DeepCOpTree::open()
{
    for (auto op : _opTree)
    {
        op->open();
    }
}

bool DeepCOpTree::validate(bool for_real)
{
    if (_opTree.empty())
    {
        return false;
    }
    DeepOnlyOp* lastOp = dynamic_cast<DeepOnlyOp*>(_opTree.back());
    if (lastOp == nullptr)
    {
        return false;
    }
    lastOp->DeepOp::validate(for_real);
    return true;
}

const DeepInfo& DeepCOpTree::deepInfo()
{
    if (_opTree.empty())
    {
        return _deepInfo;
    }
    DeepOnlyOp* lastOp = dynamic_cast<DeepOnlyOp*>(_opTree.back());
    if (lastOp == nullptr)
    {
        return _deepInfo;
    }
    return lastOp->deepInfo();
}

DD::Image::Op* DeepCOpTree::back()
{
    if (_opTree.empty())
    {
        return nullptr;
    }
    return _opTree.back();
}



bool DeepCOpTree::empty()
{
    return _opTree.empty();
}

bool DeepCOpTree::isValid()
{
    if (_opTree.empty())
    {
        return false;
    }
    const std::size_t opsAmount = _opTree.size();
    for (std::size_t opIndex=0; opIndex < opsAmount; ++opIndex)
    {
        if (_opTree[opIndex] == nullptr)
        {
            return false;
        }
    }
    return true;
}
