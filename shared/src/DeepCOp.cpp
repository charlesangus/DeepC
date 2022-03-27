#include "DeepCOp.hpp"

using namespace DD::Image;

//knob name string literals
static const char* const Z_DO_BLUR_TEXT = "do z blur";
static const char* const Z_BLUR_RADIUS_TEXT = "z blur radius";
static const char* const Z_SCALE_TEXT = "z scale";

static const char* const DROP_HIDDEN_SAMPLES_TEXT = "drop hidden samples";
static const char* const DROP_REDUNDANT_SAMPLES_TEXT = "drop redundant samples";
static const char* const MINIMUM_COLOUR_THRESHOLD_TEXT = "minimum color threshold";
static const char* const MINIMUM_ALPHA_THRESHOLD_TEXT = "minimum alpha threshold";
static const char* const MERGE_CLOSE_SAMPLE_TEXT = "merge close samples";
static const char* const MINIMUM_DISTANCE_THRESHOLD_TEXT = "minimum distance threshold";

template<typename DeepSpecT>
DeepCOp<DeepSpecT>::DeepCOp(Node* node) :
	DeepOnlyOp(node),
	_dropHiddenSamples(false),
	_dropRedundantSamples(false),
	_minimumColourThreshold(0.0001f),
	_minimumAlphaThreshold(0.01f),
	_mergeCloseSamples(false),
	_minimumDistanceThreshold(0.0001f)
{

}
template<typename DeepSpecT>
DeepCOp<DeepSpecT>::~DeepCOp()
{
}

template<typename DeepSpecT>
int DeepCOp<DeepSpecT>::knob_changed(Knob* k)
{
	//advancedBlurParameters
	knob(Z_BLUR_RADIUS_TEXT)->enable(_deepCSpec.doZBlur);
	knob(Z_SCALE_TEXT)->enable(_deepCSpec.doZBlur);

	//sampleOptimisationTab
	//knob(MINIMUM_COLOUR_THRESHOLD_TEXT)->enable(_dropHiddenSamples);
	//knob(MINIMUM_ALPHA_THRESHOLD_TEXT)->enable(_dropHiddenSamples);
	//knob(MINIMUM_DISTANCE_THRESHOLD_TEXT)->enable(_mergeCloseSamples);

	return 1;
}

template<typename DeepSpecT>
void DeepCOp<DeepSpecT>::advancedBlurParameters(Knob_Callback f)
{
	Tab_knob(f, "Advanced blur parameters");

	Bool_knob(f, &_deepCSpec.doZBlur, Z_DO_BLUR_TEXT, Z_DO_BLUR_TEXT);
	SetFlags(f, Knob::STARTLINE);
	Tooltip(f, "Whether all samples should be transformed into volumetric samples that represents a 3D blur rather than a normal 2D blur");

	Int_knob(f, &_deepCSpec.zKernelRadius, Z_BLUR_RADIUS_TEXT, Z_BLUR_RADIUS_TEXT);
	SetRange(f, 1.0f, 10.0f);
	Tooltip(f, "The radius of the z blur kernel");
	if (!_deepCSpec.doZBlur)
	{
		SetFlags(f, Knob::DISABLED);
	}

	Float_knob(f, &_deepCSpec.zScale, Z_SCALE_TEXT, Z_SCALE_TEXT);
	Tooltip(f, "The scale of the Z axis such that this value corresponds to 1 unit in depth, analogous to the width/height of a pixel being 1 unit in the X and Y axis");
	if (!_deepCSpec.doZBlur)
	{
		SetFlags(f, Knob::DISABLED | Knob::LOG_SLIDER);
	}
	else
	{
		SetFlags(f, Knob::LOG_SLIDER);
	}
	SetRange(f, 0.0001f, 1000.0f);
}

template<typename DeepSpecT>
void DeepCOp<DeepSpecT>::sampleOptimisationTab(Knob_Callback f)
{
	Tab_knob(f, "Sample Optimisations");

	Bool_knob(f, &_dropHiddenSamples, DROP_HIDDEN_SAMPLES_TEXT, DROP_HIDDEN_SAMPLES_TEXT);
	Tooltip(f, "Whether samples that are behind opaque samples should be dropped from the image");

	Divider(f, "");

	Bool_knob(f, &_dropRedundantSamples, DROP_REDUNDANT_SAMPLES_TEXT, DROP_REDUNDANT_SAMPLES_TEXT);
	Tooltip(f, "Whether samples that meet certain color and alpha thresholds should be dropped from the image");
	SetFlags(f, Knob::STARTLINE);

	Float_knob(f, &_minimumColourThreshold, MINIMUM_COLOUR_THRESHOLD_TEXT, MINIMUM_COLOUR_THRESHOLD_TEXT);
	Tooltip(f, "If the Red, Green and Blue channel values are all below this number then this threshold has been met");
	if (!_dropRedundantSamples)
	{
		SetFlags(f, Knob::DISABLED | Knob::LOG_SLIDER);
	}
	else
	{
		SetFlags(f, Knob::LOG_SLIDER);
	}
	SetRange(f, 0.000001f, 0.1f);

	Float_knob(f, &_minimumAlphaThreshold, MINIMUM_ALPHA_THRESHOLD_TEXT, MINIMUM_ALPHA_THRESHOLD_TEXT);
	Tooltip(f, "If the Alpha channel value is below this number then this threshold has been met");
	if (!_dropRedundantSamples)
	{
		SetFlags(f, Knob::DISABLED | Knob::LOG_SLIDER);
	}
	else
	{
		SetFlags(f, Knob::LOG_SLIDER);
	}
	
	Divider(f, "");

	Bool_knob(f, &_mergeCloseSamples, MERGE_CLOSE_SAMPLE_TEXT, MERGE_CLOSE_SAMPLE_TEXT);
	Tooltip(f, "Whether samples that are within the minimum distance threshold should be merged into a single sample");
	SetFlags(f, Knob::STARTLINE);

	Float_knob(f, &_minimumDistanceThreshold, MINIMUM_DISTANCE_THRESHOLD_TEXT, MINIMUM_DISTANCE_THRESHOLD_TEXT);
	Tooltip(f, "The largest depth distance between samples before they are merged. Merging done in batches from back to front so there is no guarantee adjacent samples will be merged into the same final sample");
	if (!_mergeCloseSamples)
	{
		SetFlags(f, Knob::DISABLED | Knob::LOG_SLIDER);
	}
	else
	{
		SetFlags(f, Knob::LOG_SLIDER);
	}
	SetRange(f, 0.000001f, 0.1f);
}