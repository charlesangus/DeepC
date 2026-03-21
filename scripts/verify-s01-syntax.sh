#!/usr/bin/env bash
# verify-s01-syntax.sh — Syntax-only compilation check for DeepC*.cpp
# Uses mock DDImage headers (minimal stubs) to verify C++ syntax without
# the full Nuke SDK. Exits 0 on success, 1 on failure.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SCRIPT_DIR}/../src"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# --- Create mock DDImage headers ---

mkdir -p "$TMPDIR/DDImage"

# --- Op.h (base class) ---
cat > "$TMPDIR/DDImage/Op.h" << 'HEADER'
#pragma once
#include <vector>
namespace DD { namespace Image {

class Node;
class Knob;

class Knob;
typedef void (*Knob_Callback)(void);

class Op {
public:
    Op() {}
    Op(Node*) {}
    virtual ~Op() {}
    bool aborted() const { return false; }
    Node* node() const { return nullptr; }
    virtual const char* Class() const { return ""; }
    virtual const char* node_help() const { return ""; }
    virtual void knobs(Knob_Callback) {}
    virtual Op* op() { return this; }
    void inputs(int) {}
    Op* input(int) const { return nullptr; }
    virtual int minimum_inputs() const { return 1; }
    virtual int maximum_inputs() const { return 1; }
    virtual const char* input_label(int, char*) const { return ""; }
    virtual bool test_input(int, Op*) const { return true; }
    struct Description {
        Description(const char*, const char*, Op*(*)(Node*)) {}
    };
};

}} // namespace DD::Image
HEADER

# --- Channel.h ---
cat > "$TMPDIR/DDImage/Channel.h" << 'HEADER'
#pragma once
namespace DD { namespace Image {

typedef int Channel;
static const Channel Chan_DeepFront = 0;
static const Channel Chan_DeepBack = 1;
static const Channel Chan_Alpha = 2;
static const Channel Chan_Red = 3;
static const Channel Chan_Green = 4;
static const Channel Chan_Blue = 5;

class ChannelSet {
public:
    ChannelSet() : _first(0), _count(0) {}
    int size() const { return _count; }
    Channel first() const { return _first; }
    Channel next(Channel z) const { return (z + 1 < _first + _count) ? z + 1 : 0; }
    ChannelSet& operator+=(Channel) { return *this; }
private:
    Channel _first;
    int _count;
};

}} // namespace DD::Image

// DDImage foreach macro — iterates channels in a ChannelSet
#define foreach(VAR, SET) \
    for (DD::Image::Channel VAR = (SET).first(); VAR; VAR = (SET).next(VAR))
HEADER

# --- Box.h ---
cat > "$TMPDIR/DDImage/Box.h" << 'HEADER'
#pragma once
namespace DD { namespace Image {

class Box {
public:
    Box() : _x(0), _y(0), _r(0), _t(0) {}
    Box(int x, int y, int r, int t) : _x(x), _y(y), _r(r), _t(t) {}
    int x() const { return _x; }
    int y() const { return _y; }
    int r() const { return _r; }
    int t() const { return _t; }
    void set(int x, int y, int r, int t) { _x=x; _y=y; _r=r; _t=t; }

    struct iterator {
        int x, y;
        int _x0, _r, _t;
        iterator(int x0, int y0, int r, int t) : x(x0), y(y0), _x0(x0), _r(r), _t(t) {}
        bool operator!=(const iterator& o) const { return x != o.x || y != o.y; }
        iterator& operator++() {
            ++x;
            if (x >= _r) { x = _x0; ++y; }
            return *this;
        }
    };
    iterator begin() const { return iterator(_x, _y, _r, _t); }
    iterator end() const { return iterator(_x, _t, _r, _t); }

private:
    int _x, _y, _r, _t;
};

}} // namespace DD::Image
HEADER

# --- DeepPixel.h ---
cat > "$TMPDIR/DDImage/DeepPixel.h" << 'HEADER'
#pragma once
#include "Channel.h"
namespace DD { namespace Image {

class DeepPixel {
public:
    enum Ordering { eUnordered = 0, eOrdered = 1 };
    DeepPixel() {}
    int getSampleCount() const { return 0; }
    float getUnorderedSample(int s, Channel z) const { return 0.0f; }
    float getOrderedSample(int s, Channel z) const { return 0.0f; }
};

}} // namespace DD::Image
HEADER

# --- DeepPlane.h ---
cat > "$TMPDIR/DDImage/DeepPlane.h" << 'HEADER'
#pragma once
#include "Box.h"
#include "Channel.h"
#include "DeepPixel.h"
#include <vector>
namespace DD { namespace Image {

class DeepOutPixel {
public:
    DeepOutPixel() {}
    void clear() {}
    void reserve(int) {}
    void push_back(float) {}
    int size() const { return 0; }
};

class DeepOutputPlane {
public:
    DeepOutputPlane() {}
    DeepOutputPlane(const ChannelSet&, const Box&, DeepPixel::Ordering) {}
    void addPixel(const DeepOutPixel&) {}
    void addHole() {}
};

class DeepPlane {
public:
    DeepPlane() {}
    const Box& box() const { static Box b; return b; }
    DeepPixel getPixel(int y, int x) const { return DeepPixel(); }
    DeepPixel getPixel(const Box::iterator&) const { return DeepPixel(); }
};

}} // namespace DD::Image
HEADER

# --- DeepFilterOp.h ---
cat > "$TMPDIR/DDImage/DeepFilterOp.h" << 'HEADER'
#pragma once
#include "Op.h"
#include "Box.h"
#include "Channel.h"
#include "DeepPixel.h"
#include "DeepPlane.h"
#include "Knobs.h"
#include <vector>
namespace DD { namespace Image {

class DeepOp {
public:
    bool deepEngine(const Box&, const ChannelSet&, DeepPlane&) { return true; }
};

struct RequestData {
    RequestData(DeepOp*, const Box&, const ChannelSet&, int) {}
};

struct DeepInfo {
    Box& box() { static Box b; return b; }
};

class DeepFilterOp : public Op {
public:
    DeepFilterOp(Node* n) : Op(n) {}
    virtual ~DeepFilterOp() {}
    DeepOp* input0() { return &_in; }
    bool test_input(int input, Op* op) const override { return true; }

    virtual void _validate(bool) {}
    virtual void getDeepRequests(Box, const ChannelSet&, int, std::vector<RequestData>&) {}
    virtual bool doDeepEngine(Box, const ChannelSet&, DeepOutputPlane&) { return true; }

protected:
    DeepInfo _deepInfo;
private:
    DeepOp _in;
};

}} // namespace DD::Image
HEADER

# --- Knobs.h ---
cat > "$TMPDIR/DDImage/Knobs.h" << 'HEADER'
#pragma once
namespace DD { namespace Image {

class Knob;
typedef void (*Knob_Callback)(void);

// Stub knob creation functions — signatures match DDImage API
inline Knob* Float_knob(Knob_Callback, float*, const char*, const char* = nullptr) { return nullptr; }
inline Knob* Int_knob(Knob_Callback, int*, const char*, const char* = nullptr) { return nullptr; }
inline Knob* Enumeration_knob(Knob_Callback, int*, const char* const*, const char*, const char* = nullptr) { return nullptr; }
inline void SetRange(Knob_Callback, float, float) {}
inline void SetRange(Knob_Callback, double, double) {}
inline void SetRange(Knob_Callback, int, int) {}
inline void Tooltip(Knob_Callback, const char*) {}
inline Knob* WH_knob(Knob_Callback, double*, const char*, const char* = nullptr) { return nullptr; }
inline Knob* Bool_knob(Knob_Callback, bool*, const char*, const char* = nullptr) { return nullptr; }
inline void BeginClosedGroup(Knob_Callback, const char*) {}
inline void EndGroup(Knob_Callback) {}

}} // namespace DD::Image
HEADER

# --- Run syntax-only compilation ---
for src_file in DeepCBlur.cpp DeepCDepthBlur.cpp; do
    if [ -f "$SRC_DIR/$src_file" ]; then
        echo "Running syntax check: g++ -std=c++17 -fsyntax-only -I$TMPDIR $SRC_DIR/$src_file"
        g++ -std=c++17 -fsyntax-only -I"$TMPDIR" "$SRC_DIR/$src_file"
        echo "Syntax check passed: $src_file"
    fi
done
echo "All syntax checks passed."
