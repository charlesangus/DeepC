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
    virtual Op* default_input(int) const { return nullptr; }
    virtual int knob_changed(Knob*) { return 0; }
    void error(const char*, ...) {}
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

enum Channel {
    Chan_DeepFront = 0,
    Chan_DeepBack = 1,
    Chan_Alpha = 2,
    Chan_Red = 3,
    Chan_Green = 4,
    Chan_Blue = 5
};

typedef unsigned int ChannelMask;
static const ChannelMask Mask_None = 0;
static const ChannelMask Mask_RGBA = 0xF;
static const ChannelMask Mask_All  = 0xFFFFFFFF;

class ChannelSet {
public:
    ChannelSet() : _first(static_cast<Channel>(0)), _count(0) {}
    ChannelSet(ChannelMask) : _first(static_cast<Channel>(0)), _count(4) {}
    int size() const { return _count; }
    Channel first() const { return _first; }
    Channel next(Channel z) const { int n = static_cast<int>(z) + 1; return (n < static_cast<int>(_first) + _count) ? static_cast<Channel>(n) : static_cast<Channel>(0); }
    ChannelSet& operator+=(Channel) { return *this; }
    ChannelSet operator+(Channel) const { return *this; }
    ChannelSet operator+(const ChannelSet&) const { return *this; }
    bool contains(Channel) const { return true; }
private:
    Channel _first;
    int _count;
};

}} // namespace DD::Image

// DDImage foreach macro — iterates channels in a ChannelSet
#define foreach(VAR, SET) \
    for (DD::Image::Channel VAR = (SET).first(); static_cast<int>(VAR) != 0; VAR = (SET).next(VAR))
HEADER

# --- Box.h ---
cat > "$TMPDIR/DDImage/Box.h" << 'HEADER'
#pragma once
namespace DD { namespace Image {

class Format {
public:
    Format() {}
    int width()  const { return 1920; }
    int height() const { return 1080; }
};

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

# --- Knobs.h ---
cat > "$TMPDIR/DDImage/Knobs.h" << 'HEADER'
#pragma once
namespace DD { namespace Image {

class Knob {
public:
    const char* name() const { return ""; }
};
typedef void (*Knob_Callback)(void);

// Stub knob creation functions — signatures match DDImage API
inline Knob* Float_knob(Knob_Callback, float*, const char*, const char* = nullptr) { return nullptr; }
inline Knob* Int_knob(Knob_Callback, int*, const char*, const char* = nullptr) { return nullptr; }
inline Knob* Enumeration_knob(Knob_Callback, int*, const char* const*, const char*, const char* = nullptr) { return nullptr; }
// File_knob: second param is const char** (pointer-to-const-char-pointer), matching NDK signature
inline Knob* File_knob(Knob_Callback, const char**, const char*, const char* = nullptr) { return nullptr; }
inline void SetRange(Knob_Callback, float, float) {}
inline void SetRange(Knob_Callback, double, double) {}
inline void SetRange(Knob_Callback, int, int) {}
inline void Tooltip(Knob_Callback, const char*) {}
inline Knob* WH_knob(Knob_Callback, double*, const char*, const char* = nullptr) { return nullptr; }
inline Knob* Bool_knob(Knob_Callback, bool*, const char*, const char* = nullptr) { return nullptr; }
inline void BeginClosedGroup(Knob_Callback, const char*) {}
inline void Divider(Knob_Callback, const char*) {}
inline void EndGroup(Knob_Callback) {}

}} // namespace DD::Image
HEADER

# --- Iop.h ---
cat > "$TMPDIR/DDImage/Iop.h" << 'HEADER'
#pragma once
#include "Op.h"
#include "Box.h"
#include "Channel.h"
namespace DD { namespace Image {

class Info : public Box {
public:
    Info() {}
    void set(const Box& b) { Box::set(b.x(), b.y(), b.r(), b.t()); }
    void set(const Box& b, int /*pad*/) { set(b); }
    void channels(ChannelMask) {}
    void full_size_format(const Format&) {}
    const Format& full_size_format() const { static Format f; return f; }
    void format(const Format&) {}
};

class Iop : public Op {
public:
    Iop(Node* n) : Op(n) {}
    virtual ~Iop() {}
    virtual void _validate(bool) {}
    virtual void _close() {}
    virtual int knob_changed(Knob* k) override { return Op::knob_changed(k); }
    void set_out_channels(ChannelMask) {}
    void copy_info() {}
    void validate(bool b) { _validate(b); }

    Info info_;
};

}} // namespace DD::Image
HEADER

# --- ImagePlane.h ---
cat > "$TMPDIR/DDImage/ImagePlane.h" << 'HEADER'
#pragma once
#include "Box.h"
#include "Channel.h"
namespace DD { namespace Image {

class ImagePlane {
public:
    ImagePlane() {}
    const Box& bounds() const { static Box b; return b; }
    const ChannelSet& channels() const { static ChannelSet c; return c; }
    void makeWritable() {}
    float& writableAt(int /*x*/, int /*y*/, Channel /*z*/) {
        static float dummy = 0.0f;
        return dummy;
    }
    float& writableAt(int /*x*/, int /*y*/, int /*chanIdx*/) {
        static float dummy = 0.0f;
        return dummy;
    }
    int chanNo(Channel z) const { return static_cast<int>(z); }
    float* writable(Channel, int /*y*/) { return nullptr; }
};

}} // namespace DD::Image
HEADER

# --- PlanarIop.h ---
cat > "$TMPDIR/DDImage/PlanarIop.h" << 'HEADER'
#pragma once
#include "Iop.h"
#include "ImagePlane.h"
namespace DD { namespace Image {

struct RequestOutput {};

class PlanarIop : public Iop {
public:
    PlanarIop(Node* n) : Iop(n) {}
    virtual ~PlanarIop() {}
    virtual void renderStripe(ImagePlane& plane) = 0;
    virtual void getRequests(const Box&, const ChannelSet&, int, RequestOutput&) const {}
    virtual int knob_changed(Knob* k) override { return Iop::knob_changed(k); }
    virtual void _close() override { Iop::_close(); }
};

}} // namespace DD::Image
HEADER

# --- DeepOp.h ---
cat > "$TMPDIR/DDImage/DeepOp.h" << 'HEADER'
#pragma once
#include "Op.h"
#include "Box.h"
#include "Channel.h"
#include "DeepPixel.h"
#include "DeepPlane.h"
namespace DD { namespace Image {

struct DeepInfo {
    Box& box() { static Box b; return b; }
    const Box& box() const { static Box b; return b; }
    const Format* fullSizeFormat() const { static Format f; return &f; }
    const Format* format() const { static Format f; return &f; }
};

class DeepOp {
public:
    virtual ~DeepOp() {}
    virtual bool deepEngine(const Box&, const ChannelSet&, DeepPlane&) { return true; }
    void validate(bool) {}
    const DeepInfo& deepInfo() const { static DeepInfo di; return di; }
    void deepRequest(const Box&, const ChannelSet&, int) {}
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
#include "DeepOp.h"
#include "Knobs.h"
#include <vector>
namespace DD { namespace Image {

struct RequestData {
    RequestData(DeepOp*, const Box&, const ChannelSet&, int) {}
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

# --- Run syntax-only compilation ---
for src_file in DeepCBlur.cpp DeepCDepthBlur.cpp DeepCDefocusPOThin.cpp DeepCDefocusPORay.cpp; do
    if [ -f "$SRC_DIR/$src_file" ]; then
        echo "Running syntax check: g++ -std=c++17 -fsyntax-only -I$TMPDIR -I$SRC_DIR $SRC_DIR/$src_file"
        g++ -std=c++17 -fsyntax-only -I"$TMPDIR" -I"$SRC_DIR" "$SRC_DIR/$src_file"
        echo "Syntax check passed: $src_file"
    fi
done
# --- S01 contracts ---
echo "--- S01 contracts ---"

# _validate format fix in both files
grep -q 'info_\.format(' "$SRC_DIR/DeepCDefocusPOThin.cpp" || { echo "FAIL: _validate format fix missing from Thin"; exit 1; }
echo "PASS: _validate format fix in Thin"
grep -q 'info_\.format(' "$SRC_DIR/DeepCDefocusPORay.cpp"  || { echo "FAIL: _validate format fix missing from Ray"; exit 1; }
echo "PASS: _validate format fix in Ray"

# New lens constant knobs on Ray
grep -q '_sensor_width'        "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _sensor_width knob missing in Ray"; exit 1; }
echo "PASS: _sensor_width knob in Ray"
grep -q '_back_focal_length'   "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _back_focal_length knob missing in Ray"; exit 1; }
echo "PASS: _back_focal_length knob in Ray"
grep -q '_outer_pupil_radius'  "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _outer_pupil_radius knob missing in Ray"; exit 1; }
echo "PASS: _outer_pupil_radius knob in Ray"
grep -q '_inner_pupil_radius'  "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _inner_pupil_radius knob missing in Ray"; exit 1; }
echo "PASS: _inner_pupil_radius knob in Ray"
grep -q '_aperture_pos'        "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _aperture_pos knob missing in Ray"; exit 1; }
echo "PASS: _aperture_pos knob in Ray"

# New math functions in deepc_po_math.h
grep -q 'pt_sample_aperture'       "$SRC_DIR/deepc_po_math.h" || { echo "FAIL: pt_sample_aperture missing from deepc_po_math.h"; exit 1; }
echo "PASS: pt_sample_aperture in deepc_po_math.h"
grep -q 'sphereToCs_full'          "$SRC_DIR/deepc_po_math.h" || { echo "FAIL: sphereToCs_full missing from deepc_po_math.h"; exit 1; }
echo "PASS: sphereToCs_full in deepc_po_math.h"
grep -q 'logarithmic_focus_search' "$SRC_DIR/deepc_po_math.h" || { echo "FAIL: logarithmic_focus_search missing from deepc_po_math.h"; exit 1; }
echo "PASS: logarithmic_focus_search in deepc_po_math.h"

# poly.h max_degree
grep -q 'max_degree' "$SRC_DIR/poly.h" || { echo "FAIL: max_degree missing from poly.h"; exit 1; }
echo "PASS: max_degree in poly.h"

# CMakeLists.txt registration for both plugins
grep -q 'DeepCDefocusPOThin' "$SRC_DIR/CMakeLists.txt" || { echo "FAIL: DeepCDefocusPOThin missing from CMakeLists.txt"; exit 1; }
echo "PASS: DeepCDefocusPOThin in CMakeLists.txt"
grep -q 'DeepCDefocusPORay' "$SRC_DIR/CMakeLists.txt" || { echo "FAIL: DeepCDefocusPORay missing from CMakeLists.txt"; exit 1; }
echo "PASS: DeepCDefocusPORay in CMakeLists.txt"

echo "All S01 contracts passed."

# --- S02 contracts ---
echo "--- S02 contracts ---"
grep -q 'pt_sample_aperture'        "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: pt_sample_aperture call missing from Ray renderStripe"; exit 1; }
echo "PASS: pt_sample_aperture called in Ray"
grep -q 'sphereToCs_full'           "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: sphereToCs_full call missing from Ray renderStripe"; exit 1; }
echo "PASS: sphereToCs_full called in Ray"
grep -q 'logarithmic_focus_search'  "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: logarithmic_focus_search call missing from Ray renderStripe"; exit 1; }
echo "PASS: logarithmic_focus_search called in Ray"
grep -q 'VIGNETTING_RETRIES'        "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: VIGNETTING_RETRIES missing from Ray"; exit 1; }
echo "PASS: VIGNETTING_RETRIES in Ray"
grep -q 'getPixel'                  "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: deep getPixel missing from Ray renderStripe"; exit 1; }
echo "PASS: getPixel in Ray"
! grep -q 'coc_norm'               "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: scatter vestige coc_norm still in Ray"; exit 1; }
echo "PASS: coc_norm removed from Ray"
echo "All S02 contracts passed."

echo "All syntax checks passed."
