/*
 * Copyright 2019 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/core/SkImageFilterTypes.h"

#include "include/core/SkAlphaType.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkBlender.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkClipOp.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkM44.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPicture.h"  // IWYU pragma: keep
#include "include/core/SkShader.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/private/base/SkDebug.h"
#include "include/private/base/SkFloatingPoint.h"
#include "src/base/SkMathPriv.h"
#include "src/base/SkVx.h"
#include "src/core/SkBitmapDevice.h"
#include "src/core/SkBlenderBase.h"
#include "src/core/SkBlurEngine.h"
#include "src/core/SkCanvasPriv.h"
#include "src/core/SkDevice.h"
#include "src/core/SkImageFilterCache.h"
#include "src/core/SkImageFilter_Base.h"
#include "src/core/SkMatrixPriv.h"
#include "src/core/SkRectPriv.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/core/SkTraceEvent.h"
#include "src/effects/colorfilters/SkColorFilterBase.h"

#include <algorithm>
#include <tuple>

namespace skif {

namespace {

// This exists to cover up issues where infinite precision would produce integers but float
// math produces values just larger/smaller than an int and roundOut/In on bounds would produce
// nearly a full pixel error. One such case is crbug.com/1313579 where the caller has produced
// near integer CTM and uses integer crop rects that would grab an extra row/column of the
// input image when using a strict roundOut.
static constexpr float kRoundEpsilon = 1e-3f;

// If m is epsilon within the form [1 0 tx], this returns true and sets out to [tx, ty]
//                                 [0 1 ty]
//                                 [0 0 1 ]
// TODO: Use this in decomposeCTM() (and possibly extend it to support is_nearly_scale_translate)
// to be a little more forgiving on matrix types during layer configuration.
bool is_nearly_integer_translation(const LayerSpace<SkMatrix>& m,
                                   LayerSpace<SkIPoint>* out=nullptr) {
    float tx = SkScalarRoundToScalar(sk_ieee_float_divide(m.rc(0,2), m.rc(2,2)));
    float ty = SkScalarRoundToScalar(sk_ieee_float_divide(m.rc(1,2), m.rc(2,2)));
    SkMatrix expected = SkMatrix::MakeAll(1.f, 0.f, tx,
                                          0.f, 1.f, ty,
                                          0.f, 0.f, 1.f);
    for (int i = 0; i < 9; ++i) {
        if (!SkScalarNearlyEqual(expected.get(i), m.get(i), kRoundEpsilon)) {
            return false;
        }
    }

    if (out) {
        *out = LayerSpace<SkIPoint>({(int) tx, (int) ty});
    }
    return true;
}

// Assumes 'image' is decal-tiled, so everything outside the image bounds but inside dstBounds is
// transparent black, in which case the returned special image may be smaller than dstBounds.
//
// If 'clampSrcIfDisjoint' is true and the image bounds do not overlap with dstBounds, the closest
// edge/corner pixels of the image will be extracted, assuming it will be tiled with kClamp.
std::pair<sk_sp<SkSpecialImage>, LayerSpace<SkIPoint>> extract_subset(
        const SkSpecialImage* image,
        LayerSpace<SkIPoint> origin,
        const LayerSpace<SkIRect>& dstBounds,
        bool clampSrcIfDisjoint=false) {
    LayerSpace<SkIRect> imageBounds(SkIRect::MakeXYWH(origin.x(), origin.y(),
                                                      image->width(), image->height()));
    imageBounds = imageBounds.relevantSubset(dstBounds, clampSrcIfDisjoint ? SkTileMode::kClamp
                                                                           : SkTileMode::kDecal);
    if (imageBounds.isEmpty()) {
        return {nullptr, {}};
    }

    // Offset the image subset directly to avoid issues negating (origin). With the prior
    // intersection (bounds - origin) will be >= 0, but (bounds + (-origin)) may not, (e.g.
    // origin is INT_MIN).
    SkIRect subset = { imageBounds.left() - origin.x(),
                       imageBounds.top() - origin.y(),
                       imageBounds.right() - origin.x(),
                       imageBounds.bottom() - origin.y() };
    SkASSERT(subset.fLeft >= 0 && subset.fTop >= 0 &&
             subset.fRight <= image->width() && subset.fBottom <= image->height());

    return {image->makeSubset(subset), imageBounds.topLeft()};
}

void decompose_transform(const SkMatrix& transform, SkPoint representativePoint,
                         SkMatrix* postScaling, SkMatrix* scaling) {
    SkSize scale;
    if (transform.decomposeScale(&scale, postScaling)) {
        *scaling = SkMatrix::Scale(scale.fWidth, scale.fHeight);
    } else {
        // Perspective, which has a non-uniform scaling effect on the filter. Pick a single scale
        // factor that best matches where the filter will be evaluated.
        SkScalar approxScale = SkMatrixPriv::DifferentialAreaScale(transform, representativePoint);
        if (SkScalarIsFinite(approxScale) && !SkScalarNearlyZero(approxScale)) {
            // Now take the sqrt to go from an area scale factor to a scaling per X and Y
            approxScale = SkScalarSqrt(approxScale);
        } else {
            // The point was behind the W = 0 plane, so don't factor out any scale.
            approxScale = 1.f;
        }
        *postScaling = transform;
        postScaling->preScale(SkScalarInvert(approxScale), SkScalarInvert(approxScale));
        *scaling = SkMatrix::Scale(approxScale, approxScale);
    }
}

std::optional<LayerSpace<SkMatrix>> periodic_axis_transform(
        SkTileMode tileMode,
        const LayerSpace<SkIRect>& crop,
        const LayerSpace<SkIRect>& output) {
    if (tileMode == SkTileMode::kClamp || tileMode == SkTileMode::kDecal) {
        // Not periodic
        return {};
    }

    // Lift crop dimensions into 64 bit so that we can combine with 'output' without worrying about
    // overflowing 32 bits.
    double cropL = (double) crop.left();
    double cropT = (double) crop.top();
    double cropWidth = crop.right() - cropL;
    double cropHeight = crop.bottom() - cropT;

    // Calculate normalized periodic coordinates of 'output' relative to the 'crop' being tiled.
    int periodL = sk_double_floor2int((output.left() - cropL) / cropWidth);
    int periodT = sk_double_floor2int((output.top() - cropT) / cropHeight);
    int periodR = sk_double_ceil2int((output.right() - cropL) / cropWidth);
    int periodB = sk_double_ceil2int((output.bottom() - cropT) / cropHeight);

    if (periodR - periodL <= 1 && periodB - periodT <= 1) {
        // The tiling pattern won't be visible, so we can draw the image without tiling and an
        // adjusted transform. We calculate the final translation in double to be exact and then
        // verify that it can round-trip as a float.
        float sx = 1.f;
        float sy = 1.f;
        double tx = -cropL;
        double ty = -cropT;

        if (tileMode == SkTileMode::kMirror) {
            // Flip image when in odd periods on each axis.
            if (periodL % 2 != 0) {
                sx = -1.f;
                tx = cropWidth - tx;
            }
            if (periodT % 2 != 0) {
                sy = -1.f;
                ty = cropHeight - ty;
            }
        }
        // Now translate by periods and make relative to crop's top left again. Given 32-bit inputs,
        // the period * dimension shouldn't overflow 64-bits.
        tx += periodL * cropWidth + cropL;
        ty += periodT * cropHeight + cropT;

        // Representing the periodic tiling as a float SkMatrix would lose the pixel precision
        // required to represent it, so don't apply this optimization.
        if (sk_double_saturate2int(tx) != (float) tx ||
            sk_double_saturate2int(ty) != (float) ty) {
            return {};
        }

        SkMatrix periodicTransform;
        periodicTransform.setScaleTranslate(sx, sy, (float) tx, (float) ty);
        return LayerSpace<SkMatrix>(periodicTransform);
    } else {
        // Both low and high edges of the crop would be visible in 'output', or a mirrored
        // boundary is visible in 'output'. Just keep the periodic tiling.
        return {};
    }
}

class RasterBackend : public Backend {
public:

    RasterBackend(const SkSurfaceProps& surfaceProps, SkColorType colorType)
            : Backend(SkImageFilterCache::Get(), surfaceProps, colorType) {}

    sk_sp<SkDevice> makeDevice(SkISize size,
                               sk_sp<SkColorSpace> colorSpace,
                               const SkSurfaceProps* props) const override {
        SkImageInfo imageInfo = SkImageInfo::Make(size,
                                                  this->colorType(),
                                                  kPremul_SkAlphaType,
                                                  std::move(colorSpace));
        return SkBitmapDevice::Create(imageInfo, props ? *props : this->surfaceProps());
    }

    sk_sp<SkSpecialImage> makeImage(const SkIRect& subset, sk_sp<SkImage> image) const override {
        return SkSpecialImages::MakeFromRaster(subset, image, this->surfaceProps());
    }

    sk_sp<SkImage> getCachedBitmap(const SkBitmap& data) const override {
        return SkImages::RasterFromBitmap(data);
    }

    const SkBlurEngine* getBlurEngine() const override { return nullptr; }
};

} // anonymous namespace

///////////////////////////////////////////////////////////////////////////////////////////////////

Backend::Backend(sk_sp<SkImageFilterCache> cache,
                 const SkSurfaceProps& surfaceProps,
                 const SkColorType colorType)
        : fCache(std::move(cache))
        , fSurfaceProps(surfaceProps)
        , fColorType(colorType) {}

Backend::~Backend() = default;

sk_sp<Backend> MakeRasterBackend(const SkSurfaceProps& surfaceProps, SkColorType colorType) {
    // TODO (skbug:14286): Remove this forcing to 8888. Many legacy image filters only support
    // N32 on CPU, but once they are implemented in terms of draws and SkSL they will support
    // all color types, like the GPU backends.
    colorType = kN32_SkColorType;

    return sk_make_sp<RasterBackend>(surfaceProps, colorType);
}

void Stats::dumpStats() const {
    SkDebugf("ImageFilter Stats:\n"
             "      # visited filters: %d\n"
             "           # cache hits: %d\n"
             "   # offscreen surfaces: %d\n"
             " # shader-clamped draws: %d\n"
             "   # shader-tiled draws: %d\n",
             fNumVisitedImageFilters,
             fNumCacheHits,
             fNumOffscreenSurfaces,
             fNumShaderClampedDraws,
             fNumShaderBasedTilingDraws);
}

void Stats::reportStats() const {
    TRACE_EVENT_INSTANT2("skia", "ImageFilter Graph Size", TRACE_EVENT_SCOPE_THREAD,
                         "count", fNumVisitedImageFilters, "cache hits", fNumCacheHits);
    TRACE_EVENT_INSTANT1("skia", "ImageFilter Surfaces", TRACE_EVENT_SCOPE_THREAD,
                         "count", fNumOffscreenSurfaces);
    TRACE_EVENT_INSTANT2("skia", "ImageFilter Shader Tiling", TRACE_EVENT_SCOPE_THREAD,
                         "clamp", fNumShaderClampedDraws, "other", fNumShaderBasedTilingDraws);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Mapping

SkIRect RoundOut(SkRect r) { return r.makeInset(kRoundEpsilon, kRoundEpsilon).roundOut(); }

SkIRect RoundIn(SkRect r) { return r.makeOutset(kRoundEpsilon, kRoundEpsilon).roundIn(); }

bool Mapping::decomposeCTM(const SkMatrix& ctm, MatrixCapability capability,
                           const skif::ParameterSpace<SkPoint>& representativePt) {
    SkMatrix remainder, layer;
    if (capability == MatrixCapability::kTranslate) {
        // Apply the entire CTM post-filtering
        remainder = ctm;
        layer = SkMatrix::I();
    } else if (ctm.isScaleTranslate() || capability == MatrixCapability::kComplex) {
        // Either layer space can be anything (kComplex) - or - it can be scale+translate, and the
        // ctm is. In both cases, the layer space can be equivalent to device space.
        remainder = SkMatrix::I();
        layer = ctm;
    } else {
        // This case implies some amount of sampling post-filtering, either due to skew or rotation
        // in the original matrix. As such, keep the layer matrix as simple as possible.
        decompose_transform(ctm, SkPoint(representativePt), &remainder, &layer);
    }

    SkMatrix invRemainder;
    if (!remainder.invert(&invRemainder)) {
        // Under floating point arithmetic, it's possible to decompose an invertible matrix into
        // a scaling matrix and a remainder and have the remainder be non-invertible. Generally
        // when this happens the scale factors are so large and the matrix so ill-conditioned that
        // it's unlikely that any drawing would be reasonable, so failing to make a layer is okay.
        return false;
    } else {
        fParamToLayerMatrix = layer;
        fLayerToDevMatrix = remainder;
        fDevToLayerMatrix = invRemainder;
        return true;
    }
}

bool Mapping::decomposeCTM(const SkMatrix& ctm,
                           const SkImageFilter* filter,
                           const skif::ParameterSpace<SkPoint>& representativePt) {
    return this->decomposeCTM(
            ctm,
            filter ? as_IFB(filter)->getCTMCapability() : MatrixCapability::kComplex,
            representativePt);
}

bool Mapping::adjustLayerSpace(const SkMatrix& layer) {
    SkMatrix invLayer;
    if (!layer.invert(&invLayer)) {
        return false;
    }
    fParamToLayerMatrix.postConcat(layer);
    fDevToLayerMatrix.postConcat(layer);
    fLayerToDevMatrix.preConcat(invLayer);
    return true;
}

// Instantiate map specializations for the 6 geometric types used during filtering
template<>
SkRect Mapping::map<SkRect>(const SkRect& geom, const SkMatrix& matrix) {
    return geom.isEmpty() ? SkRect::MakeEmpty() : matrix.mapRect(geom);
}

template<>
SkIRect Mapping::map<SkIRect>(const SkIRect& geom, const SkMatrix& matrix) {
    if (geom.isEmpty()) {
        return SkIRect::MakeEmpty();
    }
    // Unfortunately, there is a range of integer values such that we have 1px precision as an int,
    // but less precision as a float. This can lead to non-empty SkIRects becoming empty simply
    // because of float casting. If we're already dealing with a float rect or having a float
    // output, that's what we're stuck with; but if we are starting form an irect and desiring an
    // SkIRect output, we go through efforts to preserve the 1px precision for simple transforms.
    if (matrix.isScaleTranslate()) {
        double l = (double)matrix.getScaleX()*geom.fLeft   + (double)matrix.getTranslateX();
        double r = (double)matrix.getScaleX()*geom.fRight  + (double)matrix.getTranslateX();
        double t = (double)matrix.getScaleY()*geom.fTop    + (double)matrix.getTranslateY();
        double b = (double)matrix.getScaleY()*geom.fBottom + (double)matrix.getTranslateY();
        return {sk_double_saturate2int(sk_double_floor(std::min(l, r) + kRoundEpsilon)),
                sk_double_saturate2int(sk_double_floor(std::min(t, b) + kRoundEpsilon)),
                sk_double_saturate2int(sk_double_ceil(std::max(l, r)  - kRoundEpsilon)),
                sk_double_saturate2int(sk_double_ceil(std::max(t, b)  - kRoundEpsilon))};
    } else {
        return RoundOut(matrix.mapRect(SkRect::Make(geom)));
    }
}

template<>
SkIPoint Mapping::map<SkIPoint>(const SkIPoint& geom, const SkMatrix& matrix) {
    SkPoint p = SkPoint::Make(SkIntToScalar(geom.fX), SkIntToScalar(geom.fY));
    matrix.mapPoints(&p, 1);
    return SkIPoint::Make(SkScalarRoundToInt(p.fX), SkScalarRoundToInt(p.fY));
}

template<>
SkPoint Mapping::map<SkPoint>(const SkPoint& geom, const SkMatrix& matrix) {
    SkPoint p;
    matrix.mapPoints(&p, &geom, 1);
    return p;
}

template<>
Vector Mapping::map<Vector>(const Vector& geom, const SkMatrix& matrix) {
    SkVector v = SkVector::Make(geom.fX, geom.fY);
    matrix.mapVectors(&v, 1);
    return Vector{v};
}

template<>
IVector Mapping::map<IVector>(const IVector& geom, const SkMatrix& matrix) {
    SkVector v = SkVector::Make(SkIntToScalar(geom.fX), SkIntToScalar(geom.fY));
    matrix.mapVectors(&v, 1);
    return IVector(SkScalarRoundToInt(v.fX), SkScalarRoundToInt(v.fY));
}

// Sizes are also treated as non-positioned values (although this assumption breaks down if there's
// perspective). Unlike vectors, we treat input sizes as specifying lengths of the local X and Y
// axes and return the lengths of those mapped axes.
template<>
SkSize Mapping::map<SkSize>(const SkSize& geom, const SkMatrix& matrix) {
    if (matrix.isScaleTranslate()) {
        // This is equivalent to mapping the two basis vectors and calculating their lengths.
        SkVector sizes = matrix.mapVector(geom.fWidth, geom.fHeight);
        return {SkScalarAbs(sizes.fX), SkScalarAbs(sizes.fY)};
    }

    SkVector xAxis = matrix.mapVector(geom.fWidth, 0.f);
    SkVector yAxis = matrix.mapVector(0.f, geom.fHeight);
    return {xAxis.length(), yAxis.length()};
}

template<>
SkISize Mapping::map<SkISize>(const SkISize& geom, const SkMatrix& matrix) {
    SkSize size = map(SkSize::Make(geom), matrix);
    return SkISize::Make(SkScalarCeilToInt(size.fWidth - kRoundEpsilon),
                         SkScalarCeilToInt(size.fHeight - kRoundEpsilon));
}

template<>
SkMatrix Mapping::map<SkMatrix>(const SkMatrix& m, const SkMatrix& matrix) {
    // If 'matrix' maps from the C1 coord space to the C2 coord space, and 'm' is a transform that
    // operates on, and outputs to, the C1 coord space, we want to return a new matrix that is
    // equivalent to 'm' that operates on and outputs to C2. This is the same as mapping the input
    // from C2 to C1 (matrix^-1), then transforming by 'm', and then mapping from C1 to C2 (matrix).
    SkMatrix inv;
    SkAssertResult(matrix.invert(&inv));
    inv.postConcat(m);
    inv.postConcat(matrix);
    return inv;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// LayerSpace<T>

LayerSpace<SkIRect> LayerSpace<SkIRect>::relevantSubset(const LayerSpace<SkIRect> dstRect,
                                                        SkTileMode tileMode) const {
    LayerSpace<SkIRect> fittedSrc = *this;
    if (tileMode == SkTileMode::kDecal || tileMode == SkTileMode::kClamp) {
        // For both decal/clamp, we only care about the region that is in dstRect, unless we are
        // clamping and have to preserve edge pixels when there's no overlap.
        if (!fittedSrc.intersect(dstRect)) {
            if (tileMode == SkTileMode::kDecal) {
                // The dstRect would be filled with transparent black.
                fittedSrc = LayerSpace<SkIRect>::Empty();
            } else {
                // We just need the closest row/column/corner of this rect to dstRect.
                auto edge = SkRectPriv::ClosestDisjointEdge(SkIRect(fittedSrc),  SkIRect(dstRect));
                fittedSrc = LayerSpace<SkIRect>(edge);
            }
        }
    } // else assume the entire source is needed for periodic tile modes, so leave fittedSrc alone

    return fittedSrc;
}

// Match rounding tolerances of SkRects to SkIRects
LayerSpace<SkISize> LayerSpace<SkSize>::round() const {
    return LayerSpace<SkISize>(fData.toRound());
}
LayerSpace<SkISize> LayerSpace<SkSize>::ceil() const {
    return LayerSpace<SkISize>({SkScalarCeilToInt(fData.fWidth - kRoundEpsilon),
                                SkScalarCeilToInt(fData.fHeight - kRoundEpsilon)});
}
LayerSpace<SkISize> LayerSpace<SkSize>::floor() const {
    return LayerSpace<SkISize>({SkScalarFloorToInt(fData.fWidth + kRoundEpsilon),
                                SkScalarFloorToInt(fData.fHeight + kRoundEpsilon)});
}

LayerSpace<SkRect> LayerSpace<SkMatrix>::mapRect(const LayerSpace<SkRect>& r) const {
    return LayerSpace<SkRect>(Mapping::map(SkRect(r), fData));
}

// Effectively mapRect(SkRect).roundOut() but more accurate when the underlying matrix or
// SkIRect has large floating point values.
LayerSpace<SkIRect> LayerSpace<SkMatrix>::mapRect(const LayerSpace<SkIRect>& r) const {
    return LayerSpace<SkIRect>(Mapping::map(SkIRect(r), fData));
}

LayerSpace<SkPoint> LayerSpace<SkMatrix>::mapPoint(const LayerSpace<SkPoint>& p) const {
    return LayerSpace<SkPoint>(Mapping::map(SkPoint(p), fData));
}

LayerSpace<Vector> LayerSpace<SkMatrix>::mapVector(const LayerSpace<Vector>& v) const {
    return LayerSpace<Vector>(Mapping::map(Vector(v), fData));
}

LayerSpace<SkSize> LayerSpace<SkMatrix>::mapSize(const LayerSpace<SkSize>& s) const {
    return LayerSpace<SkSize>(Mapping::map(SkSize(s), fData));
}

bool LayerSpace<SkMatrix>::inverseMapRect(const LayerSpace<SkRect>& r,
                                          LayerSpace<SkRect>* out) const {
    SkRect mapped;
    if (r.isEmpty()) {
        // An empty input always inverse maps to an empty rect "successfully"
        *out = LayerSpace<SkRect>::Empty();
        return true;
    } else if (SkMatrixPriv::InverseMapRect(fData, &mapped, SkRect(r))) {
        *out = LayerSpace<SkRect>(mapped);
        return true;
    } else {
        return false;
    }
}

bool LayerSpace<SkMatrix>::inverseMapRect(const LayerSpace<SkIRect>& rect,
                                          LayerSpace<SkIRect>* out) const {
    if (rect.isEmpty()) {
        // An empty input always inverse maps to an empty rect "successfully"
        *out = LayerSpace<SkIRect>::Empty();
        return true;
    } else if (fData.isScaleTranslate()) { // Specialized inverse of 1px-preserving map<SkIRect>
        // A scale-translate matrix with a 0 scale factor is not invertible.
        if (fData.getScaleX() == 0.f || fData.getScaleY() == 0.f) {
            return false;
        }
        double l = (rect.left()   - (double)fData.getTranslateX()) / (double)fData.getScaleX();
        double r = (rect.right()  - (double)fData.getTranslateX()) / (double)fData.getScaleX();
        double t = (rect.top()    - (double)fData.getTranslateY()) / (double)fData.getScaleY();
        double b = (rect.bottom() - (double)fData.getTranslateY()) / (double)fData.getScaleY();

        SkIRect mapped{sk_double_saturate2int(sk_double_floor(std::min(l, r) + kRoundEpsilon)),
                       sk_double_saturate2int(sk_double_floor(std::min(t, b) + kRoundEpsilon)),
                       sk_double_saturate2int(sk_double_ceil(std::max(l, r)  - kRoundEpsilon)),
                       sk_double_saturate2int(sk_double_ceil(std::max(t, b)  - kRoundEpsilon))};
        *out = LayerSpace<SkIRect>(mapped);
        return true;
    } else {
        SkRect mapped;
        if (SkMatrixPriv::InverseMapRect(fData, &mapped, SkRect::Make(SkIRect(rect)))) {
            *out = LayerSpace<SkRect>(mapped).roundOut();
            return true;
        }
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FilterResult::AutoSurface
//
// AutoSurface manages an SkCanvas and device state to draw to a layer-space bounding box,
// and then snap it into a FilterResult. It provides operators to be used directly as an SkDevice,
// assuming surface creation succeeded. It can also be viewed as an SkCanvas (for when an operation
// is unavailable on SkDevice). A given AutoSurface should only rely on one access API.
// Usage:
//
//     AutoSurface surface{ctx, dstBounds, renderInParameterSpace}; // if true, concats layer matrix
//     if (surface) {
//         surface->drawFoo(...);
//     }
//     return surface.snap(); // Automatically handles failed allocations
class FilterResult::AutoSurface {
public:
    AutoSurface(const Context& ctx,
                const LayerSpace<SkIRect>& dstBounds,
                bool renderInParameterSpace,
                const SkSurfaceProps* props = nullptr)
            : fDstBounds(dstBounds) {
        // We don't intersect by ctx.desiredOutput() and only use the Context to make the surface.
        // It is assumed the caller has already accounted for the desired output, or it's a
        // situation where the desired output shouldn't apply (e.g. this surface will be transformed
        // to align with the actual desired output via FilterResult metadata).
        ctx.markNewSurface();
        sk_sp<SkDevice> device =
                dstBounds.isEmpty() ? nullptr
                                    : ctx.backend()->makeDevice(SkISize(dstBounds.size()),
                                                                ctx.refColorSpace(),
                                                                props);
        if (!device) {
            return;
        }

        // Wrap the device in a canvas and use that to configure its origin and clip. This ensures
        // the device and the canvas are in sync regardless of how the AutoSurface user intends
        // to render.
        fCanvas.emplace(std::move(device));
        fCanvas->translate(-fDstBounds.left(), -fDstBounds.top());
        fCanvas->clear(SkColors::kTransparent);
        // The device functor may have provided an approx-fit backing surface so clip to the
        // expected dst bounds.
        fCanvas->clipIRect(SkIRect(fDstBounds));

        if (renderInParameterSpace) {
            fCanvas->concat(SkMatrix(ctx.mapping().layerMatrix()));
        }
    }

    explicit operator bool() const { return fCanvas.has_value(); }

    SkDevice* device() { SkASSERT(fCanvas.has_value()); return SkCanvasPriv::TopDevice(&*fCanvas); }
    SkCanvas* operator->() { SkASSERT(fCanvas.has_value()); return &*fCanvas; }

    // NOTE: This pair is equivalent to a FilterResult but we keep it this way for use by resolve(),
    // which wants them separate while the legacy imageAndOffset() function is around.
    std::pair<sk_sp<SkSpecialImage>, LayerSpace<SkIPoint>> snap() {
        if (fCanvas.has_value()) {
            // Snap a subset of the device matching the expected dst bounds.
            SkIRect subset = SkIRect::MakeWH(fDstBounds.width(), fDstBounds.height());
            fCanvas->restoreToCount(0);
            this->device()->setImmutable();
            auto snapped = std::make_pair(this->device()->snapSpecial(subset),
                                          fDstBounds.topLeft());
            fCanvas.reset(); // Only use the AutoSurface once
            return snapped;
        } else {
            return {nullptr, {}};
        }
    }

private:
    std::optional<SkCanvas> fCanvas;
    LayerSpace<SkIRect> fDstBounds;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// FilterResult

sk_sp<SkSpecialImage> FilterResult::imageAndOffset(const Context& ctx, SkIPoint* offset) const {
    auto [image, origin] = this->resolve(ctx, fLayerBounds);
    *offset = SkIPoint(origin);
    return image;
}

std::pair<sk_sp<SkSpecialImage>, LayerSpace<SkIPoint>>FilterResult::imageAndOffset(
        const Context& ctx) const {
    return this->resolve(ctx, fLayerBounds);
}

SkEnumBitMask<FilterResult::BoundsAnalysis> FilterResult::analyzeBounds(
        const SkMatrix& xtraTransform,
        const SkIRect& dstBounds) const {
    static constexpr SkSamplingOptions kNearestNeighbor = {};
    static constexpr float kHalfPixel = 0.5f;
    static constexpr float kCubicRadius = 1.5f;

    SkEnumBitMask<BoundsAnalysis> analysis = BoundsAnalysis::kSimple;
    const bool fillsLayerBounds = fTileMode != SkTileMode::kDecal ||
                                  (fColorFilter && as_CFB(fColorFilter)->affectsTransparentBlack());

    // 1. Is the layer geometry visible in the dstBounds (ignoring whether or not there are shading
    //    effects that highlight that boundary).
    SkRect pixelCenterBounds = SkRect::Make(dstBounds);
    if (!SkRectPriv::QuadContainsRect(xtraTransform,
                                      SkIRect(fLayerBounds),
                                      dstBounds,
                                      kRoundEpsilon)) {
        // 1a. If an effect doesn't fill out to the layer bounds, is the image content itself
        //     clipped by the layer bounds?
        bool requireLayerCrop = fillsLayerBounds;
        if (!fillsLayerBounds) {
            LayerSpace<SkIRect> imageBounds =
                    fTransform.mapRect(LayerSpace<SkIRect>{fImage->dimensions()});
            requireLayerCrop = !fLayerBounds.contains(imageBounds);
        }

        if (requireLayerCrop) {
            analysis |= BoundsAnalysis::kRequiresLayerCrop;
            // And since the layer crop will have to be applied externally, we can restrict the
            // sample bounds to the intersection of dstBounds and layerBounds
            SkIRect layerBoundsInDst = Mapping::map(SkIRect(fLayerBounds), xtraTransform);
            // In some cases these won't intersect, usually in a complex graph where the input is
            // a bitmap or the dynamic source, in which case it hasn't been clipped or dropped by
            // earlier image filter processing for that particular node. We could return a flag here
            // to signal that the operation should be treated as transparent black, but that would
            // create more shader combinations and image sampling will still do the right thing by
            // leaving 'pixelCenterBounds' as the original 'dstBounds'.
            (void) pixelCenterBounds.intersect(SkRect::Make(layerBoundsInDst));
        }
        // else this is a decal-tiled, non-transparent affecting FilterResult that doesn't have
        // its pixel data clipped by the layer bounds, so the layer crop doesn't have to be applied
        // separately. But this means that the image will be sampled over all of 'dstBounds'.
    }
    // else the layer bounds geometry isn't visible, so 'dstBounds' is already a tighter bounding
    // box for how the image will be sampled.

    // 2. Are the tiling and deferred color filter effects visible in the sampled bounds
    SkRect imageBounds = SkRect::Make(fImage->dimensions());
    LayerSpace<SkMatrix> netTransform = fTransform;
    netTransform.postConcat(LayerSpace<SkMatrix>(xtraTransform));
    SkM44 netM44{SkMatrix(netTransform)};

    if (!SkRectPriv::QuadContainsRect(netM44,
                                      imageBounds,
                                      pixelCenterBounds,
                                      kRoundEpsilon)) {
        analysis |= BoundsAnalysis::kDstBoundsNotCovered;
        if (fillsLayerBounds) {
            analysis |= BoundsAnalysis::kHasLayerFillingEffect;
        }
    }

    // 3. Would image pixels outside of its subset be sampled if shader-clamping is skipped?
    const float sampleRadius = fSamplingOptions.useCubic ? kCubicRadius : kHalfPixel;
    SkRect safeImageBounds = imageBounds.makeInset(sampleRadius, sampleRadius);
    if (fSamplingOptions == kDefaultSampling && !is_nearly_integer_translation(netTransform)) {
        // When using default sampling, integer translations are eventually downgraded to nearest
        // neighbor, so the 1/2px inset clamping is sufficient to safely access within the subset.
        // When staying with linear filtering, a sample at 1/2px inset exactly will end up accessing
        // one external pixel with a weight of 0 (but MSAN will complain and not all GPUs actually
        // seem to get that correct). To be safe we have to clamp to epsilon inside the 1/2px.
        safeImageBounds.inset(kRoundEpsilon, kRoundEpsilon);
    }
    pixelCenterBounds.inset(kHalfPixel, kHalfPixel);

    // True if all corners of 'pixelCenterBounds' are on the inside of each edge of
    // 'safeImageBounds', ordered T,R,B,L.
    skvx::int4 edgeMask = SkRectPriv::QuadContainsRectMask(netM44,
                                                           safeImageBounds,
                                                           pixelCenterBounds,
                                                           kRoundEpsilon);
    if (!all(edgeMask)) {
        // Sampling outside the image subset occurs, but if the edges that are exceeded are HW
        // edges, then we can avoid using shader-based tiling.
        skvx::int4 hwEdge{fImage->subset().fTop == 0,
                          fImage->subset().fRight == fImage->backingStoreDimensions().fWidth,
                          fImage->subset().fBottom == fImage->backingStoreDimensions().fHeight,
                          fImage->subset().fLeft == 0};
        if (fTileMode == SkTileMode::kRepeat || fTileMode == SkTileMode::kMirror) {
            // For periodic tile modes, we require both edges on an axis to be HW edges
            hwEdge = hwEdge & skvx::shuffle<2,3,0,1>(hwEdge); // TRBL & BLTR
        }
        if (!all(edgeMask | hwEdge)) {
            analysis |= BoundsAnalysis::kRequiresShaderTiling;
        }

        if (fTileMode == SkTileMode::kDecal) {
            // Some amount of decal tiling will be visible in the output, but it only needs to
            // be handled special if it's not nearest neighbor and not an identity scale factor.
            // NOTE: all the cases where fSamplingOptions is not nearest neighbor, but can be
            // reduced to nearest neighbor later, satisfy the net xform having the identity scale
            float scaleFactors[2];
            if (fSamplingOptions != kNearestNeighbor &&
                !(SkMatrix(netTransform).getMinMaxScales(scaleFactors) &&
                  SkScalarNearlyEqual(scaleFactors[0], 1.f, 0.2f) &&
                  SkScalarNearlyEqual(scaleFactors[1], 1.f, 0.2f))) {
                analysis |= BoundsAnalysis::kRequiresDecalInLayerSpace;
            }
        }
    }

    return analysis;
}

void FilterResult::updateTileMode(const Context& ctx, SkTileMode tileMode) {
    if (fImage) {
        fTileMode = tileMode;
        if (tileMode != SkTileMode::kDecal) {
            fLayerBounds = ctx.desiredOutput();
        }
    }
}

FilterResult FilterResult::applyCrop(const Context& ctx,
                                     const LayerSpace<SkIRect>& crop,
                                     SkTileMode tileMode) const {
    static const LayerSpace<SkMatrix> kIdentity{SkMatrix::I()};

    if (crop.isEmpty() || ctx.desiredOutput().isEmpty()) {
        // An empty crop cannot be anything other than fully transparent
        return {};
    }

    // First, determine how this image's layer bounds interact with the crop rect, which determines
    // the portion of 'crop' that could have non-transparent content.
    LayerSpace<SkIRect> cropContent = crop;
    if (!fImage ||
        !cropContent.intersect(fLayerBounds)) {
        // The pixels within 'crop' would be fully transparent, and tiling won't change that.
        return {};
    }

    // Second, determine the subset of 'crop' that is relevant to ctx.desiredOutput().
    LayerSpace<SkIRect> fittedCrop = crop.relevantSubset(ctx.desiredOutput(), tileMode);

    // Third, check if there's overlap with the known non-transparent cropped content and what's
    // used to tile the desired output. If not, the image is known to be empty. This modifies
    // 'cropContent' and not 'fittedCrop' so that any transparent padding remains if we have to
    // apply repeat/mirror tiling to the original geometry.
    if (!cropContent.intersect(fittedCrop)) {
        return {};
    }

    // Fourth, a periodic tiling that covers the output with a single instance of the image can be
    // simplified to just a transform.
    auto periodicTransform = periodic_axis_transform(tileMode, fittedCrop, ctx.desiredOutput());
    if (periodicTransform) {
        return this->applyTransform(ctx, *periodicTransform, FilterResult::kDefaultSampling);
    }

    bool preserveTransparencyInCrop = false;
    if (tileMode == SkTileMode::kDecal) {
        // We can reduce the crop dimensions to what's non-transparent
        fittedCrop = cropContent;
    } else if (fittedCrop.contains(ctx.desiredOutput())) {
        tileMode = SkTileMode::kDecal;
        fittedCrop = ctx.desiredOutput();
    } else if (!cropContent.contains(fittedCrop)) {
        // There is transparency in fittedCrop that must be resolved in order to maintain the new
        // tiling geometry.
        preserveTransparencyInCrop = true;
        if (fTileMode == SkTileMode::kDecal && tileMode == SkTileMode::kClamp) {
            // include 1px buffer for transparency from original kDecal tiling
            cropContent.outset(skif::LayerSpace<SkISize>({1, 1}));
            SkAssertResult(fittedCrop.intersect(cropContent));
        }
    } // Otherwise cropContent == fittedCrop

    // Fifth, when the transform is an integer translation, any prior tiling and the new tiling
    // can sometimes be addressed analytically without producing a new image. Moving the crop into
    // the image dimensions allows future operations like applying a transform or color filter to
    // be composed without rendering a new image since there will not be an intervening crop.
    const bool doubleClamp = fTileMode == SkTileMode::kClamp && tileMode == SkTileMode::kClamp;
    LayerSpace<SkIPoint> origin;
    if (!preserveTransparencyInCrop &&
        is_nearly_integer_translation(fTransform, &origin) &&
        (doubleClamp ||
         !(this->analyzeBounds(fittedCrop) & BoundsAnalysis::kHasLayerFillingEffect))) {
        // Since the transform is axis-aligned, the tile mode can be applied to the original
        // image pre-transformation and still be consistent with the 'crop' geometry. When the
        // original tile mode is decal, extract_subset is always valid. When the original mode is
        // mirror/repeat, !kHasLayerFillingEffect ensures that 'fittedCrop' is contained within
        // the base image bounds, so extract_subset is valid. When the original mode is clamp
        // and the new mode is not clamp, that is also the case. When both modes are clamp, we have
        // to consider how 'fittedCrop' intersects (or doesn't) with the base image bounds.
        FilterResult restrictedOutput =
                extract_subset(fImage.get(), origin, fittedCrop, doubleClamp);
        // This does not rely on resolve() to call extract_subset() because it  will still render a
        // new image if there's a color filter. As such, we have to preserve the current color
        // filter on the new FilterResult.
        restrictedOutput.fColorFilter = fColorFilter;
        restrictedOutput.updateTileMode(ctx, tileMode);
        return restrictedOutput;
    } else if (tileMode == SkTileMode::kDecal) {
        // A decal crop can always be applied as the final operation by adjusting layer bounds, and
        // does not modify any prior tile mode.
        SkASSERT(!preserveTransparencyInCrop);
        FilterResult restrictedOutput = *this;
        restrictedOutput.fLayerBounds = fittedCrop;
        return restrictedOutput;
    } else {
        // There is a non-trivial transform to the image data that must be applied before the
        // non-decal tilemode is meant to be applied to the axis-aligned 'crop'.
        FilterResult tiled = this->resolve(ctx, fittedCrop, true);
        tiled.updateTileMode(ctx, tileMode);
        return tiled;
    }
}

FilterResult FilterResult::applyColorFilter(const Context& ctx,
                                            sk_sp<SkColorFilter> colorFilter) const {
    // A null filter is the identity, so it should have been caught during image filter DAG creation
    SkASSERT(colorFilter);

    if (ctx.desiredOutput().isEmpty()) {
        return {};
    }

    // Color filters are applied after the transform and image sampling, but before the fLayerBounds
    // crop. We can compose 'colorFilter' with any previously applied color filter regardless
    // of the transform/sample state, so long as it respects the effect of the current crop.
    LayerSpace<SkIRect> newLayerBounds = fLayerBounds;
    if (as_CFB(colorFilter)->affectsTransparentBlack()) {
        if (!fImage || !newLayerBounds.intersect(ctx.desiredOutput())) {
            // The current image's intersection with the desired output is fully transparent, but
            // the new color filter converts that into a non-transparent color. The desired output
            // is filled with this color, but use a 1x1 surface and clamp tiling.
            AutoSurface surface{ctx,
                                LayerSpace<SkIRect>{SkIRect::MakeXYWH(ctx.desiredOutput().left(),
                                                                      ctx.desiredOutput().top(),
                                                                      1, 1)},
                                /*renderInParameterSpace=*/false};
            if (surface) {
                SkPaint paint;
                paint.setColor4f(SkColors::kTransparent, /*colorSpace=*/nullptr);
                paint.setColorFilter(std::move(colorFilter));
                surface->drawPaint(paint);
            }
            FilterResult solidColor = surface.snap();
            solidColor.updateTileMode(ctx, SkTileMode::kClamp);
            return solidColor;
        }

        if (this->analyzeBounds(ctx.desiredOutput()) & BoundsAnalysis::kRequiresLayerCrop) {
            // Since 'colorFilter' modifies transparent black, the new result's layer bounds must
            // be the desired output. But if the current image is cropped we need to resolve the
            // image to avoid losing the effect of the current 'fLayerBounds'.
            newLayerBounds.outset(LayerSpace<SkISize>({1, 1}));
            SkAssertResult(newLayerBounds.intersect(ctx.desiredOutput()));
            FilterResult filtered = this->resolve(ctx, newLayerBounds,
                                                  /*preserveTransparency=*/true);
            filtered.fColorFilter = std::move(colorFilter);
            filtered.updateTileMode(ctx, SkTileMode::kClamp);
            return filtered;
        }

        // otherwise we can fill out to the desired output without worrying about losing the crop.
        newLayerBounds = ctx.desiredOutput();
    } else {
        if (!fImage || !newLayerBounds.intersect(ctx.desiredOutput())) {
            // The color filter does not modify transparent black, so it remains transparent
            return {};
        }
        // otherwise a non-transparent affecting color filter can always be lifted before any crop
        // because it does not change the "shape" of the prior FilterResult.
    }

    // If we got here we can compose the new color filter with the previous filter and the prior
    // layer bounds are either soft-cropped to the desired output, or we fill out the desired output
    // when the new color filter affects transparent black. We don't check if the entire composed
    // filter affects transparent black because earlier floods are restricted by the layer bounds.
    FilterResult filtered = *this;
    filtered.fLayerBounds = newLayerBounds;
    filtered.fColorFilter = SkColorFilters::Compose(std::move(colorFilter), fColorFilter);
    return filtered;
}

static bool compatible_sampling(const SkSamplingOptions& currentSampling,
                                bool currentXformWontAffectNearest,
                                SkSamplingOptions* nextSampling,
                                bool nextXformWontAffectNearest) {
    // Both transforms could perform non-trivial sampling, but if they are similar enough we
    // assume performing one non-trivial sampling operation with the concatenated transform will
    // not be visually distinguishable from sampling twice.
    // TODO(michaelludwig): For now ignore mipmap policy, SkSpecialImages are not supposed to be
    // drawn with mipmapping, and the majority of filter steps produce images that are at the
    // proper scale and do not define mip levels. The main exception is the ::Image() filter
    // leaf but that doesn't use this system yet.
    if (currentSampling.isAniso() && nextSampling->isAniso()) {
        // Assume we can get away with one sampling at the highest anisotropy level
        *nextSampling =  SkSamplingOptions::Aniso(std::max(currentSampling.maxAniso,
                                                           nextSampling->maxAniso));
        return true;
    } else if (currentSampling.isAniso() && nextSampling->filter == SkFilterMode::kLinear) {
        // Assume we can get away with the current anisotropic filter since the next is linear
        *nextSampling = currentSampling;
        return true;
    } else if (nextSampling->isAniso() && currentSampling.filter == SkFilterMode::kLinear) {
        // Mirror of the above, assume we can just get away with next's anisotropic filter
        return true;
    } else if (currentSampling.useCubic && (nextSampling->filter == SkFilterMode::kLinear ||
                                            (nextSampling->useCubic &&
                                             currentSampling.cubic.B == nextSampling->cubic.B &&
                                             currentSampling.cubic.C == nextSampling->cubic.C))) {
        // Assume we can get away with the current bicubic filter, since the next is the same
        // or a bilerp that can be upgraded.
        *nextSampling = currentSampling;
        return true;
    } else if (nextSampling->useCubic && currentSampling.filter == SkFilterMode::kLinear) {
        // Mirror of the above, assume we can just get away with next's cubic resampler
        return true;
    } else if (currentSampling.filter == SkFilterMode::kLinear &&
               nextSampling->filter == SkFilterMode::kLinear) {
        // Assume we can get away with a single bilerp vs. the two
        return true;
    } else if (nextSampling->filter == SkFilterMode::kNearest && currentXformWontAffectNearest) {
        // The next transform and nearest-neighbor filtering isn't impacted by the current transform
        SkASSERT(currentSampling.filter == SkFilterMode::kLinear);
        return true;
    } else if (currentSampling.filter == SkFilterMode::kNearest && nextXformWontAffectNearest) {
        // The next transform doesn't change the nearest-neighbor filtering of the current transform
        SkASSERT(nextSampling->filter == SkFilterMode::kLinear);
        *nextSampling = currentSampling;
        return true;
    } else {
        // The current or next sampling is nearest neighbor, and will produce visible texels
        // oriented with the current transform; assume this is a desired effect and preserve it.
        return false;
    }
}

FilterResult FilterResult::applyTransform(const Context& ctx,
                                          const LayerSpace<SkMatrix> &transform,
                                          const SkSamplingOptions &sampling) const {
    if (!fImage || ctx.desiredOutput().isEmpty()) {
        // Transformed transparent black remains transparent black.
        SkASSERT(!fColorFilter);
        return {};
    }

    // Extract the sampling options that matter based on the current and next transforms.
    // We make sure the new sampling is bilerp (default) if the new transform doesn't matter
    // (and assert that the current is bilerp if its transform didn't matter). Bilerp can be
    // maximally combined, so simplifies the logic in compatible_sampling().
    const bool currentXformIsInteger = is_nearly_integer_translation(fTransform);
    const bool nextXformIsInteger = is_nearly_integer_translation(transform);

    SkASSERT(!currentXformIsInteger || fSamplingOptions == kDefaultSampling);
    SkSamplingOptions nextSampling = nextXformIsInteger ? kDefaultSampling : sampling;

    // Determine if the image is being visibly cropped by the layer bounds, in which case we can't
    // merge this transform with any previous transform (unless the new transform is an integer
    // translation in which case any visible edge is aligned with the desired output and can be
    // resolved by intersecting the transformed layer bounds and the output bounds).
    bool isCropped = !nextXformIsInteger &&
                     (this->analyzeBounds(SkMatrix(transform), SkIRect(ctx.desiredOutput()))
                            & BoundsAnalysis::kRequiresLayerCrop);

    FilterResult transformed;
    if (!isCropped && compatible_sampling(fSamplingOptions, currentXformIsInteger,
                                          &nextSampling, nextXformIsInteger)) {
        // We can concat transforms and 'nextSampling' will be either fSamplingOptions,
        // sampling, or a merged combination depending on the two transforms in play.
        transformed = *this;
    } else {
        // We'll have to resolve this FilterResult first before 'transform' and 'sampling' can be
        // correctly evaluated. 'nextSampling' will always be 'sampling'.
        LayerSpace<SkIRect> tightBounds;
        if (transform.inverseMapRect(ctx.desiredOutput(), &tightBounds)) {
            transformed = this->resolve(ctx, tightBounds);
        }

        if (!transformed.fImage) {
            // Transform not invertible or resolve failed to create an image
            return {};
        }
    }

    transformed.fSamplingOptions = nextSampling;
    transformed.fTransform.postConcat(transform);
    // Rebuild the layer bounds and then restrict to the current desired output. The original value
    // of fLayerBounds includes the image mapped by the original fTransform as well as any
    // accumulated soft crops from desired outputs of prior stages. To prevent discarding that info,
    // we map fLayerBounds by the additional transform, instead of re-mapping the image bounds.
    transformed.fLayerBounds = transform.mapRect(transformed.fLayerBounds);
    if (!transformed.fLayerBounds.intersect(ctx.desiredOutput())) {
        // The transformed output doesn't touch the desired, so it would just be transparent black.
        // TODO: This intersection only applies when the tile mode is kDecal.
        return {};
    }

    return transformed;
}

std::pair<sk_sp<SkSpecialImage>, LayerSpace<SkIPoint>> FilterResult::resolve(
        const Context& ctx,
        LayerSpace<SkIRect> dstBounds,
        bool preserveTransparency) const {
    // The layer bounds is the final clip, so it can always be used to restrict 'dstBounds'. Even
    // if there's a non-decal tile mode or transparent-black affecting color filter, those floods
    // are restricted to fLayerBounds.
    if (!fImage || (!preserveTransparency && !dstBounds.intersect(fLayerBounds))) {
        return {nullptr, {}};
    }

    // If we have any extra effect to apply, there's no point in trying to extract a subset.
    const bool subsetCompatible = !fColorFilter &&
                                  fTileMode == SkTileMode::kDecal &&
                                  !preserveTransparency;

    // TODO(michaelludwig): If we get to the point where all filter results track bounds in
    // floating point, then we can extend this case to any S+T transform.
    LayerSpace<SkIPoint> origin;
    if (subsetCompatible && is_nearly_integer_translation(fTransform, &origin)) {
        return extract_subset(fImage.get(), origin, dstBounds);
    } // else fall through and attempt a draw

    // Don't use context properties to avoid DMSAA on internal stages of filter evaluation.
    SkSurfaceProps props = {};
    AutoSurface surface{ctx, dstBounds, /*renderInParameterSpace=*/false, &props};
    if (surface) {
        this->draw(ctx, surface.device(), /*preserveDeviceState=*/false);
    }
    return surface.snap();
}

void FilterResult::draw(const Context& ctx, SkDevice* target, const SkBlender* blender) const {
    SkAutoDeviceTransformRestore adtr{target, ctx.mapping().layerToDevice()};
    this->draw(ctx, target, /*preserveDeviceState=*/true, blender);
}

void FilterResult::draw(const Context& ctx,
                        SkDevice* device,
                        bool preserveDeviceState,
                        const SkBlender* blender) const {
    const bool blendAffectsTransparentBlack = blender && as_BB(blender)->affectsTransparentBlack();
    if (!fImage) {
        // The image is transparent black, this is a no-op unless we need to apply the blend mode
        if (blendAffectsTransparentBlack) {
            SkPaint clear;
            clear.setColor4f(SkColors::kTransparent);
            clear.setBlender(sk_ref_sp(blender));
            device->drawPaint(clear);
        }
        return;
    }

    SkEnumBitMask<BoundsAnalysis> analysis = this->analyzeBounds(device->localToDevice(),
                                                                 device->devClipBounds());

    if (analysis & BoundsAnalysis::kRequiresLayerCrop) {
        if (blendAffectsTransparentBlack) {
            // This is similar to the resolve() path in applyColorFilter() when the filter affects
            // transparent black but must be applied after the prior visible layer bounds clip.
            // NOTE: We map devClipBounds() by the local-to-device matrix instead of the Context
            // mapping because that works for both use cases: drawing to the final device (where
            // the transforms are the same), or drawing to intermediate layer images (where they
            // are not the same).
            LayerSpace<SkIRect> dstBounds;
            if (!LayerSpace<SkMatrix>(device->localToDevice()).inverseMapRect(
                        LayerSpace<SkIRect>(device->devClipBounds()), &dstBounds)) {
                return;
            }
            // Regardless of the scenario, the end result is that it's in layer space.
            FilterResult clipped = this->resolve(ctx, dstBounds);
            clipped.draw(ctx, device, preserveDeviceState, blender);
            return;
        }
        // Otherwise we can apply the layer bounds as a clip to avoid an intermediate render pass
        if (preserveDeviceState) {
            device->pushClipStack();
        }
        device->clipRect(SkRect::Make(SkIRect(fLayerBounds)), SkClipOp::kIntersect, /*aa=*/true);
    }

    // If we are an integer translate, the default bilinear sampling *should* be equivalent to
    // nearest-neighbor. Going through the direct image-drawing path tends to detect this
    // and reduce sampling automatically. When we have to use an image shader, this isn't
    // detected and some GPUs' linear filtering doesn't exactly match nearest-neighbor and can
    // lead to leaks beyond the image's subset. Detect and reduce sampling explicitly.
    SkSamplingOptions sampling = fSamplingOptions;
    if (sampling == kDefaultSampling &&
        is_nearly_integer_translation(fTransform) &&
        is_nearly_integer_translation(skif::LayerSpace<SkMatrix>(device->localToDevice()))) {
        sampling = {};
    }

    SkPaint paint;
    paint.setAntiAlias(true);
    if (blender) {
        paint.setBlender(sk_ref_sp(blender));
    } else {
        paint.setBlendMode(SkBlendMode::kSrcOver);
    }

    if (analysis & BoundsAnalysis::kHasLayerFillingEffect ||
        (blendAffectsTransparentBlack && (analysis & BoundsAnalysis::kDstBoundsNotCovered))) {
        // Fill the canvas with the shader, so that the pixels beyond the image dimensions are still
        // covered by the draw and either resolve tiling into the image, color filter transparent
        // black, apply the blend mode to the dst, or any combination thereof.
        paint.setShader(this->getAnalyzedShaderView(ctx, sampling, analysis));
        device->drawPaint(paint);
    } else {
        // src's origin is embedded in fTransform. For historical reasons, drawSpecial() does
        // not automatically use the device's current local-to-device matrix, but that's what preps
        // it to match the expected layer coordinate system.
        paint.setColorFilter(fColorFilter);
        SkMatrix netTransform = SkMatrix::Concat(device->localToDevice(), SkMatrix(fTransform));

        SkCanvas::SrcRectConstraint constraint = SkCanvas::kFast_SrcRectConstraint;
        if (analysis & BoundsAnalysis::kRequiresShaderTiling) {
            constraint = SkCanvas::kStrict_SrcRectConstraint;
            ctx.markShaderBasedTilingRequired(SkTileMode::kClamp);
        }
        device->drawSpecial(fImage.get(), netTransform, sampling, paint, constraint);
    }

    if (preserveDeviceState && (analysis & BoundsAnalysis::kRequiresLayerCrop)) {
        device->popClipStack();
    }
}

sk_sp<SkShader> FilterResult::asShader(const Context& ctx,
                                       const SkSamplingOptions& xtraSampling,
                                       SkEnumBitMask<ShaderFlags> flags,
                                       const LayerSpace<SkIRect>& sampleBounds) const {
    if (!fImage) {
        return nullptr;
    }
    // Even if flags don't force resolving the filter result to an axis-aligned image, if the
    // extra sampling to be applied is not compatible with the accumulated transform and sampling,
    // or if the logical image is cropped by the layer bounds, the FilterResult will need to be
    // resolved to an image before we wrap it as an SkShader. When checking if cropped, we use the
    // FilterResult's layer bounds instead of the context's desired output, assuming that the layer
    // bounds reflect the bounds of the coords a parent shader will pass to eval().
    const bool currentXformIsInteger = is_nearly_integer_translation(fTransform);
    const bool nextXformIsInteger = !(flags & ShaderFlags::kNonTrivialSampling);

    SkBlendMode colorFilterMode;
    SkEnumBitMask<BoundsAnalysis> analysis = this->analyzeBounds(sampleBounds);

    SkSamplingOptions sampling = xtraSampling;
    const bool needsResolve =
            // Deferred calculations on the input would be repeated with each sample, but we allow
            // simple color filters to skip resolving since their repeated math should be cheap.
            (flags & ShaderFlags::kSampledRepeatedly &&
                    ((fColorFilter && (!fColorFilter->asAColorMode(nullptr, &colorFilterMode) ||
                                       colorFilterMode > SkBlendMode::kLastCoeffMode)) ||
                     !SkColorSpace::Equals(fImage->getColorSpace(), ctx.colorSpace()))) ||
            // The deferred sampling options can't be merged with the one requested
            !compatible_sampling(fSamplingOptions, currentXformIsInteger,
                                 &sampling, nextXformIsInteger) ||
            // The deferred edge of the layer bounds is visible to sampling
            (analysis & BoundsAnalysis::kRequiresLayerCrop);

    // Downgrade to nearest-neighbor if the sequence of sampling doesn't do anything
    if (sampling == kDefaultSampling && nextXformIsInteger &&
        (needsResolve || currentXformIsInteger)) {
        sampling = {};
    }

    sk_sp<SkShader> shader;
    if (needsResolve) {
        // The resolve takes care of fTransform (sans origin), fTileMode, fColorFilter, and
        // fLayerBounds
        auto [pixels, origin] = this->resolve(ctx, fLayerBounds);
        if (pixels) {
            shader = pixels->asShader(SkTileMode::kDecal, sampling,
                                      SkMatrix::Translate(origin.x(), origin.y()));
            if (!pixels->isExactFit()) {
                ctx.markShaderBasedTilingRequired(SkTileMode::kDecal);
            }
        }
    } else {
        shader = this->getAnalyzedShaderView(ctx, sampling, analysis);
    }

    return shader;
}

sk_sp<SkShader> FilterResult::getAnalyzedShaderView(
        const Context& ctx,
        const SkSamplingOptions& finalSampling,
        SkEnumBitMask<BoundsAnalysis> analysis) const {
    const SkMatrix& localMatrix(fTransform);
    const SkRect imageBounds = SkRect::Make(fImage->dimensions());
    // We need to apply the decal in a coordinate space that matches the resolution of the layer
    // space. If the transform preserves rectangles, map the image bounds by the transform so we
    // can apply it before we evaluate the shader. Otherwise decompose the transform into a
    // non-scaling post-decal transform and a scaling pre-decal transform.
    SkMatrix postDecal, preDecal;
    if (localMatrix.rectStaysRect() ||
        !(analysis & BoundsAnalysis::kRequiresDecalInLayerSpace)) {
        postDecal = SkMatrix::I();
        preDecal = localMatrix;
    } else {
        decompose_transform(localMatrix, imageBounds.center(), &postDecal, &preDecal);
    }

    // If the image covers the dst bounds, then its tiling won't be visible, so we can switch
    // to the faster kClamp for either HW or shader-based tiling. If we are applying the decal
    // in layer space, then that extra shader implements the tiling, so we can switch to clamp
    // for the image shader itself.
    SkTileMode effectiveTileMode = fTileMode;
    if (!(analysis & BoundsAnalysis::kDstBoundsNotCovered) ||
        analysis & BoundsAnalysis::kRequiresDecalInLayerSpace) {
        effectiveTileMode = SkTileMode::kClamp;
    }
    const bool strict = SkToBool(analysis & BoundsAnalysis::kRequiresShaderTiling);
    if (strict) {
        ctx.markShaderBasedTilingRequired(effectiveTileMode);
    }
    sk_sp<SkShader> imageShader =
            fImage->asShader(effectiveTileMode, finalSampling, preDecal, strict);

    if (analysis & BoundsAnalysis::kRequiresDecalInLayerSpace) {
        SkASSERT(fTileMode == SkTileMode::kDecal);
        // TODO(skbug:12784) - As part of fully supporting subsets in image shaders, it probably
        // makes sense to share the subset tiling logic that's in GrTextureEffect as dedicated
        // SkShaders. Graphite can then add those to its program as-needed vs. always doing
        // shader-based tiling, and CPU can have raster-pipeline tiling applied more flexibly than
        // at the bitmap level. At that point, this effect is redundant and can be replaced with the
        // decal-subset shader.
        static const SkRuntimeEffect* effect = SkMakeRuntimeEffect(SkRuntimeEffect::MakeForShader,
            "uniform shader image;"
            "uniform float4 decalBounds;"

            "half4 main(float2 coord) {"
                "half4 d = half4(decalBounds - coord.xyxy) * half4(-1, -1, 1, 1);"
                "d = saturate(d + 0.5);"
                "return (d.x*d.y*d.z*d.w) * image.eval(coord);"
            "}");

        SkRuntimeShaderBuilder builder(sk_ref_sp(effect));
        builder.child("image") = std::move(imageShader);
        builder.uniform("decalBounds") = preDecal.mapRect(imageBounds);

        imageShader = builder.makeShader();
    }

    if (imageShader && !postDecal.isIdentity()) {
        imageShader = imageShader->makeWithLocalMatrix(postDecal);
    }

    if (imageShader && fColorFilter) {
        imageShader = imageShader->makeWithColorFilter(fColorFilter);
    }

    // Shader now includes the image, the sampling, the tile mode, the transform, and the color
    // filter, skipping deferred effects that aren't present or aren't visible given 'analysis'.
    // The last "effect", layer bounds cropping, must be handled externally by either resolving
    // the image before hand or clipping the device that's drawing the returned shader.
    return imageShader;
}

static int downscale_step_count(float netScaleFactor) {
    int steps = SkNextLog2(sk_float_ceil2int(1.f / netScaleFactor));
    // There are (steps-1) 1/2x steps and then one step that will be between 1/2-1x. If the
    // final step is practically the identity scale, we can save a render pass and not incur too
    // much sampling error by reducing the step count and using a final scale that's slightly less
    // than 1/2.
    if (steps > 0) {
        // For a multipass rescale, we allow for a lot of tolerance when deciding to collapse the
        // final step. If there's only a single pass, we require the scale factor to be very close
        // to the identity since it causes the step count to go to 0.
        static constexpr float kMultiPassLimit = 0.8f;
        static constexpr float kNearIdentityLimit = 1.f - kRoundEpsilon; // 1px error in 1000px img

        float finalStepScale = netScaleFactor * (1 << (steps - 1));
        float limit = steps == 1 ? kNearIdentityLimit : kMultiPassLimit;
        if (finalStepScale >= limit) {
            steps--;
        }
    }

    return steps;
}

// The following code uses "PixelSpace" as an alias to refer to the LayerSpace of the low-res
// input image and blurred output to differentiate values for the original and final layer space
template <typename T>
using PixelSpace = LayerSpace<T>;

FilterResult FilterResult::rescale(const Context& ctx,
                                   const LayerSpace<SkSize>& scale,
                                   bool enforceDecal) const {
    LayerSpace<SkIRect> visibleLayerBounds = fLayerBounds;
    if (!fImage || !visibleLayerBounds.intersect(ctx.desiredOutput()) ||
        scale.width() <= 0.f || scale.height() <= 0.f) {
        return {};
    }

    int xSteps = downscale_step_count(scale.width());
    int ySteps = downscale_step_count(scale.height());

    // NOTE: For the first pass, PixelSpace and LayerSpace are equivalent
    PixelSpace<SkIPoint> origin;
    const bool pixelAligned = is_nearly_integer_translation(fTransform, &origin);
    SkEnumBitMask<BoundsAnalysis> analysis = this->analyzeBounds(ctx.desiredOutput());

    // If there's no actual scaling, and no other effects that have to be resolved for blur(),
    // then just extract the necessary subset. Otherwise fall through and apply the effects with
    // scale factor (possibly identity).
    const bool canDeferTiling =
            pixelAligned &&
            !(analysis & BoundsAnalysis::kRequiresLayerCrop) &&
            !(enforceDecal && (analysis & BoundsAnalysis::kHasLayerFillingEffect));

    const bool hasEffectsToApply =
            !canDeferTiling ||
            SkToBool(fColorFilter) ||
            fImage->colorType() != ctx.backend()->colorType() ||
            !SkColorSpace::Equals(fImage->getColorSpace(), ctx.colorSpace());

    if (xSteps == 0 && ySteps == 0 && !hasEffectsToApply) {
        if (analysis & BoundsAnalysis::kHasLayerFillingEffect) {
            // At this point, the only effects that could be visible is a non-decal mode, so just
            // return the image with adjusted layer bounds to match desired output.
            FilterResult noop = *this;
            noop.fLayerBounds = visibleLayerBounds;
            return noop;
        } else {
            // The visible layer bounds represents a tighter bounds than the image itself
            return extract_subset(fImage.get(), origin, visibleLayerBounds);
        }
    }

    PixelSpace<SkIRect> srcRect;
    SkTileMode tileMode;
    if (canDeferTiling && (analysis & BoundsAnalysis::kHasLayerFillingEffect)) {
        // When we can defer tiling, and said tiling is visible, rescaling the original image
        // uses smaller textures.
        srcRect = LayerSpace<SkIRect>(SkIRect::MakeXYWH(origin.x(), origin.y(),
                                                        fImage->width(), fImage->height()));
        tileMode = fTileMode;
    } else {
        // Otherwise we either have to rescale the layer-bounds-sized image (!canDeferTiling)
        // or the tiling isn't visible so the layer bounds reprenents a smaller effective
        // image than the original image data.
        srcRect = visibleLayerBounds;
        tileMode = SkTileMode::kDecal;
    }

    srcRect = srcRect.relevantSubset(ctx.desiredOutput(), tileMode);
    if (srcRect.isEmpty()) {
        return {};
    }

    // To avoid incurring error from rounding up the dimensions at every step, the logical size of
    // the image is tracked in floats through the whole process; rounding to integers is only done
    // to produce a conservative pixel buffer and clamp-tiling is used so that partially covered
    // pixels are filled with the un-weighted color.
    PixelSpace<SkRect> stepBoundsF{srcRect};
    // stepPixelBounds is used to calculate how much padding needs to be added. Adding 1px outset
    // keeps the math consistent for first iteration vs. later iterations, and logically represents
    // the first downscale triggering the tilemode vs. later steps sampling the preserved tiling
    // in the padded pixels.
    PixelSpace<SkIRect> stepPixelBounds{srcRect};
    stepPixelBounds.outset(PixelSpace<SkISize>({1, 1}));

    // If we made it here, at least one iteration is required, even if xSteps and ySteps are 0.
    sk_sp<SkSpecialImage> image = nullptr;
    while(!image || xSteps > 0 || ySteps > 0) {
        float sx = 1.f;
        if (xSteps > 0) {
            sx = xSteps > 1 ? 0.5f : srcRect.width()*scale.width() / stepBoundsF.width();
            xSteps--;
        }

        float sy = 1.f;
        if (ySteps > 0) {
            sy = ySteps > 1 ? 0.5f : srcRect.height()*scale.height() / stepBoundsF.height();
            ySteps--;
        }

        PixelSpace<SkRect> dstBoundsF{SkRect::MakeWH(stepBoundsF.width() * sx,
                                                     stepBoundsF.height() * sy)};
        PixelSpace<SkIRect> dstPixelBounds = dstBoundsF.roundOut();
        if (tileMode == SkTileMode::kClamp || tileMode == SkTileMode::kDecal) {
            // To sample beyond the padded src texel, we need
            //      dstFracX + px - 1/2 > sx*(srcFracX - 1/2)
            // px=1 always satisfies this for sx=1/2 on intermediate steps, but for 0.5 < sx < 1
            // the fractional bounds and rounding can require an additional padded pixel.
            // We calculate from the right edge because we keep the left edge pixel aligned.
            float srcFracX = stepPixelBounds.right() - stepBoundsF.right() - 0.5f;
            float dstFracX = dstPixelBounds.right()  - dstBoundsF.right()  - 0.5f;
            int px = std::max(1, sk_float_ceil2int((sx*srcFracX - dstFracX)));

            float srcFracY = stepPixelBounds.bottom() - stepBoundsF.bottom() - 0.5f;
            float dstFracY = dstPixelBounds.bottom()  - dstBoundsF.bottom()  - 0.5f;
            int py = std::max(1, sk_float_ceil2int((sy*srcFracY - dstFracY)));

            dstPixelBounds.outset(PixelSpace<SkISize>({px, py}));

            // If the axis scale factor was identity, the dst pixel bounds *after* padding will
            // match the step pixel bounds. We have to add re-add the padding on identity iterations
            // because the initial dst bounds is based on the un-padded stepBoundsF.
            SkASSERT(sx != 1.f || dstPixelBounds.width() == stepPixelBounds.width());
            SkASSERT(sy != 1.f || dstPixelBounds.height() == stepPixelBounds.height());
        }

        AutoSurface surface{ctx, dstPixelBounds, /*renderInParameterSpace=*/false};
        if (surface) {
            // Fill all of surface (to include any padded edge pixels) with 'scaleXform' as the CTM.
            const auto scaleXform = PixelSpace<SkMatrix>::RectToRect(stepBoundsF, dstBoundsF);
            surface->concat(SkMatrix(scaleXform));

            SkPaint paint;
            if (!image) {
                // Redo analysis with the actual scale transform and padded low res bounds, but
                // remove kRequiresDecalInLayerSpace because it will always trigger with the scale
                // factor and can be automatically applied at the end when upscaling.
                analysis = this->analyzeBounds(SkMatrix(scaleXform), SkIRect(dstPixelBounds));
                analysis &= ~BoundsAnalysis::kRequiresDecalInLayerSpace;
                paint.setShader(this->getAnalyzedShaderView(ctx, fSamplingOptions, analysis));
            } else {
                // Otherwise just bilinearly downsample the origin-aligned prior step's image.
                paint.setShader(image->asShader(tileMode, SkFilterMode::kLinear,
                                                SkMatrix::Translate(origin.x(), origin.y())));
                if (!image->isExactFit()) {
                    ctx.markShaderBasedTilingRequired(tileMode);
                }
            }

            surface->drawPaint(paint);
        } else {
            // Rescaling can't complete, no sense in downscaling non-existent data
            return {};
        }

        if (tileMode == SkTileMode::kDecal) {
            // Now we have incorporated a 1px transparent border, so next image can use clamping.
            // OR we have incorporated the transparency-affecting color filter's result to the
            // 1px transparent border so the next image can still use clamping.
            tileMode = SkTileMode::kClamp;
        } // else we are non-decal deferred so use repeat/mirror/clamp all the way down.

        std::tie(image, origin) = surface.snap();
        stepBoundsF = dstBoundsF;
        stepPixelBounds = dstPixelBounds;
    }

    // Rebuild the downscaled image as a FilterResult, including a transform back to the original
    // layer-space resolution, restoring the layer bounds it should fill, and setting tile mode.
    FilterResult result{std::move(image), origin};
    result.fTransform.postConcat(
            LayerSpace<SkMatrix>::RectToRect(stepBoundsF, LayerSpace<SkRect>{srcRect}));
    result.fLayerBounds = visibleLayerBounds;

    if (enforceDecal) {
        // Since we weren't deferring the tiling, the original tile mode should have been resolved
        // in the first iteration. However, as part of the decimation, we included transparent
        // padding and switched to clamp. Switching back to "decal" in this case has no visual
        // effect but keeps downstream legacy blur algorithms happy.
        SkASSERT(!canDeferTiling && tileMode == SkTileMode::kClamp);
        result.fTileMode = SkTileMode::kDecal;
    } else {
        result.fTileMode = tileMode;
    }
    return result;
}

FilterResult FilterResult::MakeFromPicture(const Context& ctx,
                                           sk_sp<SkPicture> pic,
                                           ParameterSpace<SkRect> cullRect) {
    SkASSERT(pic);
    LayerSpace<SkIRect> dstBounds = ctx.mapping().paramToLayer(cullRect).roundOut();
    if (!dstBounds.intersect(ctx.desiredOutput())) {
        return {};
    }

    // Given the standard usage of the picture image filter (i.e., to render content at a fixed
    // resolution that, most likely, differs from the screen's) disable LCD text by removing any
    // knowledge of the pixel geometry.
    // TODO: Should we just generally do this for layers with image filters? Or can we preserve it
    // for layers that are still axis-aligned?
    SkSurfaceProps props = ctx.backend()->surfaceProps()
                                         .cloneWithPixelGeometry(kUnknown_SkPixelGeometry);
    AutoSurface surface{ctx, dstBounds, /*renderInParameterSpace=*/true, &props};
    if (surface) {
        surface->clipRect(SkRect(cullRect));
        surface->drawPicture(std::move(pic));
    }
    return surface.snap();
}

FilterResult FilterResult::MakeFromShader(const Context& ctx,
                                          sk_sp<SkShader> shader,
                                          bool dither) {
    SkASSERT(shader);
    AutoSurface surface{ctx, ctx.desiredOutput(), /*renderInParameterSpace=*/true};
    if (surface) {
        SkPaint paint;
        paint.setShader(shader);
        paint.setDither(dither);
        surface->drawPaint(paint);
    }
    return surface.snap();
}

FilterResult FilterResult::MakeFromImage(const Context& ctx,
                                         sk_sp<SkImage> image,
                                         const SkRect& srcRect,
                                         const ParameterSpace<SkRect>& dstRect,
                                         const SkSamplingOptions& sampling) {
    SkASSERT(image);
    // Check for direct conversion to an SkSpecialImage and then FilterResult. Eventually this
    // whole function should be replaceable with:
    //    FilterResult(fImage, fSrcRect, fDstRect).applyTransform(mapping.layerMatrix(), fSampling);
    SkIRect srcSubset = RoundOut(srcRect);
    if (SkRect::Make(srcSubset) == srcRect) {
        // Construct an SkSpecialImage from the subset directly instead of drawing.
        sk_sp<SkSpecialImage> specialImage = ctx.backend()->makeImage(srcSubset, std::move(image));

        // Treat the srcRect's top left as "layer" space since we are folding the src->dst transform
        // and the param->layer transform into a single transform step.
        skif::FilterResult subset{std::move(specialImage),
                                  skif::LayerSpace<SkIPoint>(srcSubset.topLeft())};
        SkMatrix transform = SkMatrix::Concat(ctx.mapping().layerMatrix(),
                                              SkMatrix::RectToRect(srcRect, SkRect(dstRect)));
        return subset.applyTransform(ctx, skif::LayerSpace<SkMatrix>(transform), sampling);
    }

    // For now, draw the src->dst subset of image into a new image.
    LayerSpace<SkIRect> dstBounds = ctx.mapping().paramToLayer(dstRect).roundOut();
    if (!dstBounds.intersect(ctx.desiredOutput())) {
        return {};
    }

    AutoSurface surface{ctx, dstBounds, /*renderInParameterSpace=*/true};
    if (surface) {
        SkPaint paint;
        paint.setAntiAlias(true);
        surface->drawImageRect(std::move(image), srcRect, SkRect(dstRect), sampling, &paint,
                               SkCanvas::kStrict_SrcRectConstraint);
    }
    return surface.snap();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// FilterResult::Builder

FilterResult::Builder::Builder(const Context& context) : fContext(context) {}
FilterResult::Builder::~Builder() = default;

SkSpan<sk_sp<SkShader>> FilterResult::Builder::createInputShaders(
        const LayerSpace<SkIRect>& outputBounds,
        bool evaluateInParameterSpace) {
    SkEnumBitMask<ShaderFlags> xtraFlags = ShaderFlags::kNone;
    SkMatrix layerToParam;
    if (evaluateInParameterSpace) {
        // The FilterResult is meant to be sampled in layer space, but the shader this is feeding
        // into is being sampled in parameter space. Add the inverse of the layerMatrix() (i.e.
        // layer to parameter space) as a local matrix to convert from the parameter-space coords
        // of the outer shader to the layer-space coords of the FilterResult).
        SkAssertResult(fContext.mapping().layerMatrix().invert(&layerToParam));
        // Automatically add nonTrivial sampling if the layer-to-parameter space mapping isn't
        // also pixel aligned.
        if (!is_nearly_integer_translation(LayerSpace<SkMatrix>(layerToParam))) {
            xtraFlags |= ShaderFlags::kNonTrivialSampling;
        }
    }

    fInputShaders.reserve(fInputs.size());
    for (const SampledFilterResult& input : fInputs) {
        // Assume the input shader will be evaluated once per pixel in the output unless otherwise
        // specified when the FilterResult was added to the builder.
        auto sampleBounds = input.fSampleBounds ? *input.fSampleBounds : outputBounds;
        auto shader = input.fImage.asShader(fContext,
                                            input.fSampling,
                                            input.fFlags | xtraFlags,
                                            sampleBounds);
        if (evaluateInParameterSpace && shader) {
            shader = shader->makeWithLocalMatrix(layerToParam);
        }
        fInputShaders.push_back(std::move(shader));
    }
    return SkSpan<sk_sp<SkShader>>(fInputShaders);
}

LayerSpace<SkIRect> FilterResult::Builder::outputBounds(
        std::optional<LayerSpace<SkIRect>> explicitOutput) const {
    // Pessimistically assume output fills the full desired bounds
    LayerSpace<SkIRect> output = fContext.desiredOutput();
    if (explicitOutput.has_value()) {
        // Intersect with the provided explicit bounds
        if (!output.intersect(*explicitOutput)) {
            return LayerSpace<SkIRect>::Empty();
        }
    }
    return output;
}

FilterResult FilterResult::Builder::drawShader(sk_sp<SkShader> shader,
                                               const LayerSpace<SkIRect>& outputBounds,
                                               bool evaluateInParameterSpace) const {
    SkASSERT(!outputBounds.isEmpty()); // Should have been rejected before we created shaders
    if (!shader) {
        return {};
    }

    AutoSurface surface{fContext, outputBounds, evaluateInParameterSpace};
    if (surface) {
        SkPaint paint;
        paint.setShader(std::move(shader));
        surface->drawPaint(paint);
    }
    return surface.snap();
}

FilterResult FilterResult::Builder::merge() {
    // merge() could return an empty image on 0 added inputs, but this should have been caught
    // earlier and routed to SkImageFilters::Empty() instead.
    SkASSERT(!fInputs.empty());
    if (fInputs.size() == 1) {
        SkASSERT(!fInputs[0].fSampleBounds.has_value() &&
                 fInputs[0].fSampling == kDefaultSampling &&
                 fInputs[0].fFlags == ShaderFlags::kNone);
        return fInputs[0].fImage;
    }

    const auto mergedBounds = LayerSpace<SkIRect>::Union(
            (int) fInputs.size(),
            [this](int i) { return fInputs[i].fImage.layerBounds(); });
    const auto outputBounds = this->outputBounds(mergedBounds);

    AutoSurface surface{fContext, outputBounds, /*renderInParameterSpace=*/false};
    if (surface) {
        for (const SampledFilterResult& input : fInputs) {
            SkASSERT(!input.fSampleBounds.has_value() &&
                     input.fSampling == kDefaultSampling &&
                     input.fFlags == ShaderFlags::kNone);
            input.fImage.draw(fContext, surface.device(), /*preserveDeviceState=*/true);
        }
    }
    return surface.snap();
}

FilterResult FilterResult::Builder::blur(const LayerSpace<SkSize>& sigma) {
    SkASSERT(fInputs.size() == 1);

    // TODO: The blur functor is only supported for GPU contexts; SkBlurImageFilter should have
    // detected this.
    const SkBlurEngine* blurEngine = fContext.backend()->getBlurEngine();
    SkASSERT(blurEngine);

    // TODO: All tilemodes are applied right now in resolve() so query with just kDecal
    const SkBlurEngine::Algorithm* algorithm = blurEngine->findAlgorithm(
            SkSize(sigma), fContext.backend()->colorType());
    if (!algorithm) {
        return {};
    }

    // TODO: Move resizing logic out of GrBlurUtils into this function
    SkASSERT(sigma.width() <= algorithm->maxSigma() && sigma.height() <= algorithm->maxSigma());

    // TODO: De-duplicate this logic between SkBlurImageFilter, here, and skgpu::BlurUtils.
    skif::LayerSpace<SkISize> radii =
            LayerSpace<SkSize>({3.f*sigma.width(), 3.f*sigma.height()}).ceil();
    auto maxOutput = fInputs[0].fImage.layerBounds();
    maxOutput.outset(radii);

    // TODO: If the input image is periodic, the output that's calculated can be the original image
    // size and then have the layer bounds and tilemode of the output image apply the tile again.
    // Similarly, a clamped blur can be restricted to a radius-outset buffer of the image bounds
    // (vs. layer bounds) and rendered with clamp tiling.
    const auto outputBounds = this->outputBounds(maxOutput);
    if (outputBounds.isEmpty()) {
        return {};
    }

    // These are the source pixels that will be read from the input image, which can be calculated
    // internally because the blur's access pattern is well defined (vs. needing it to be provided
    // in Builder::add()).
    auto sampleBounds = outputBounds;
    sampleBounds.outset(radii);

    // TODO: If the blur implementation requires downsampling, we should incorporate any deferred
    // transform and colorfilter to the first rescale step instead of generating a full resolution
    // simple image first.
    // TODO: The presence of a non-decal tilemode should not force resolving to a simple image; it
    // should be incorporated into the image that's sampled by the blur effect (modulo biasing edge
    // pixels somehow for very large clamp blurs).
    // TODO: resolve() doesn't actually guarantee that the returned image has the same color space
    // as the Context, but probably should since the blur algorithm operates in the color space of
    // the input image.
    auto [image, origin] = fInputs[0].fImage.resolve(fContext, sampleBounds);
    if (!image) {
        return {};
    }

    // TODO: Can blur() take advantage of AutoSurface? Right now the GPU functions are responsible
    // for creating their own target surfaces.
    auto srcRelativeOutput = outputBounds;
    srcRelativeOutput.offset(-origin);
    image = algorithm->blur(SkSize(sigma),
                            image,
                            SkIRect::MakeSize(image->dimensions()),
                            SkTileMode::kDecal,
                            SkIRect(srcRelativeOutput));

    // TODO: Allow the blur functor to provide an upscaling transform that is applied to the
    // FilterResult so that a render pass can possibly be elided if this is the final operation.
    return {image, outputBounds.topLeft()};
}

} // end namespace skif
