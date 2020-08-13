/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/**************************************************************************************************
 *** This file was autogenerated from GrTwoPointConicalGradientLayout.fp; do not modify.
 **************************************************************************************************/
#ifndef GrTwoPointConicalGradientLayout_DEFINED
#define GrTwoPointConicalGradientLayout_DEFINED

#include "include/core/SkM44.h"
#include "include/core/SkTypes.h"

#include "src/gpu/effects/GrMatrixEffect.h"
#include "src/gpu/gradients/GrGradientShader.h"
#include "src/shaders/gradients/SkTwoPointConicalGradient.h"

#include "src/gpu/GrFragmentProcessor.h"

class GrTwoPointConicalGradientLayout : public GrFragmentProcessor {
public:
    enum class Type { kFocal = 2, kRadial = 0, kStrip = 1 };

    static std::unique_ptr<GrFragmentProcessor> Make(const SkTwoPointConicalGradient& gradient,
                                                     const GrFPArgs& args);
    GrTwoPointConicalGradientLayout(const GrTwoPointConicalGradientLayout& src);
    std::unique_ptr<GrFragmentProcessor> clone() const override;
    const char* name() const override { return "TwoPointConicalGradientLayout"; }
    Type type;
    bool isRadiusIncreasing;
    bool isFocalOnCircle;
    bool isWellBehaved;
    bool isSwapped;
    bool isNativelyFocal;
    SkPoint focalParams;

private:
    GrTwoPointConicalGradientLayout(Type type,
                                    bool isRadiusIncreasing,
                                    bool isFocalOnCircle,
                                    bool isWellBehaved,
                                    bool isSwapped,
                                    bool isNativelyFocal,
                                    SkPoint focalParams)
            : INHERITED(kGrTwoPointConicalGradientLayout_ClassID,
                        (OptimizationFlags)kNone_OptimizationFlags)
            , type(type)
            , isRadiusIncreasing(isRadiusIncreasing)
            , isFocalOnCircle(isFocalOnCircle)
            , isWellBehaved(isWellBehaved)
            , isSwapped(isSwapped)
            , isNativelyFocal(isNativelyFocal)
            , focalParams(focalParams) {
        this->setUsesSampleCoordsDirectly();
    }
    GrGLSLFragmentProcessor* onCreateGLSLInstance() const override;
    void onGetGLSLProcessorKey(const GrShaderCaps&, GrProcessorKeyBuilder*) const override;
    bool onIsEqual(const GrFragmentProcessor&) const override;
#if GR_TEST_UTILS
    SkString onDumpInfo() const override;
#endif
    GR_DECLARE_FRAGMENT_PROCESSOR_TEST
    typedef GrFragmentProcessor INHERITED;
};
#endif
