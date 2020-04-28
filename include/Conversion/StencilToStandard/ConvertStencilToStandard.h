#ifndef CONVERSION_STENCILTOSTANDARD_CONVERTSTENCILTOSTANDARD_H
#define CONVERSION_STENCILTOSTANDARD_CONVERTSTENCILTOSTANDARD_H

#include "Dialect/Stencil/StencilOps.h"
#include "mlir/Transforms/DialectConversion.h"

namespace mlir {

void populateStencilToStandardConversionPatterns(
    OwningRewritePatternList &patterns, MLIRContext *ctx);
}

#endif // CONVERSION_STENCILTOSTANDARD_CONVERTSTENCILTOSTANDARD_H
