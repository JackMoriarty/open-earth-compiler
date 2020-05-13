#include "Conversion/StencilToStandard/ConvertStencilToStandard.h"
#include "Conversion/StencilToStandard/Passes.h"
#include "Dialect/Stencil/StencilDialect.h"
#include "Dialect/Stencil/StencilOps.h"
#include "Dialect/Stencil/StencilTypes.h"
#include "Dialect/Stencil/StencilUtils.h"
#include "PassDetail.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/LoopOps/LoopOps.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Region.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/IR/Value.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/Utils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/raw_ostream.h"
#include <bits/stdint-intn.h>
#include <cstddef>
#include <iterator>

using namespace mlir;
using namespace stencil;

namespace {

// // Helper method to check if lower bound is zero
// bool isZero(ArrayRef<int64_t> offset) {
//   return llvm::all_of(offset, [](int64_t x) {
//     return x == 0;
//   });
// }

// Helper method computing the strides given the size
Index computeStrides(ArrayRef<int64_t> shape) {
  Index result(shape.size());
  result[0] = 1;
  for (size_t i = 1, e = result.size(); i != e; ++i)
    result[i] = result[i - 1] * shape[i - 1];
  return result;
}

// // Helper method computing the shape given the range
// Index computeShape(ArrayRef<int64_t> begin,
//                                      ArrayRef<int64_t> end) {
//   assert(begin.size() == end.size() && "expected bounds to have the same
//   size"); Index result(begin.size()); llvm::transform(llvm::zip(end, begin),
//   result.begin(),
//                   [](std::tuple<int64_t, int64_t> x) {
//                     return std::get<0>(x) - std::get<1>(x);
//                   });
//   // Remove ignored dimensions
//   llvm::erase_if(result, [](int64_t x) { return x == 0; });
//   return result;
// }

// Helper method computing linearizing the offset
int64_t computeOffset(ArrayRef<int64_t> offset, ArrayRef<int64_t> strides) {
  // Compute the linear offset
  int64_t result = 0;
  for (size_t i = 0, e = strides.size(); i != e; ++i) {
    result += offset[i] * strides[i];
  }
  return result;
}

// // Helper to compute a memref type
// MemRefType computeMemRefType(Type elementType, ArrayRef<int64_t> shape,
//                              ArrayRef<int64_t> strides,
//                              ArrayRef<int64_t> origin,
//                              ConversionPatternRewriter &rewriter) {
//   // Get the element type
//   int64_t offset = 0;
//   if (origin.size() != 0) {
//     offset = computeOffset(origin, strides);
//   }
//   auto map = makeStridedLinearLayoutMap(strides, offset,
//   rewriter.getContext()); return MemRefType::get(shape, elementType, map, 0);
// }

//===----------------------------------------------------------------------===//
// Rewriting Pattern
//===----------------------------------------------------------------------===//

class FuncOpLowering : public StencilOpToStdPattern<FuncOp> {
public:
  using StencilOpToStdPattern<FuncOp>::StencilOpToStdPattern;

  LogicalResult
  matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = operation->getLoc();
    auto funcOp = cast<FuncOp>(operation);

    // Convert the original function arguments
    TypeConverter::SignatureConversion result(funcOp.getNumArguments());
    for (auto &en : llvm::enumerate(funcOp.getType().getInputs()))
      result.addInputs(en.index(), typeConverter.convertType(en.value()));
    auto funcType =
        FunctionType::get(result.getConvertedTypes(),
                          funcOp.getType().getResults(), funcOp.getContext());

    // Replace the function by a function with an upadate signature
    auto newFuncOp =
        rewriter.create<FuncOp>(loc, funcOp.getName(), funcType, llvm::None);
    rewriter.inlineRegionBefore(funcOp.getBody(), newFuncOp.getBody(),
                                newFuncOp.end());

    // Convert the signature and delete the original operation
    rewriter.applySignatureConversion(&newFuncOp.getBody(), result);
    rewriter.eraseOp(funcOp);

    return success();
  }
};

class AssertOpLowering : public StencilOpToStdPattern<stencil::AssertOp> {
public:
  using StencilOpToStdPattern<stencil::AssertOp>::StencilOpToStdPattern;

  LogicalResult
  matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = operation->getLoc();
    auto assertOp = cast<stencil::AssertOp>(operation);

    auto fieldType = assertOp.field().getType().cast<FieldType>();

    // // Compute the shape
    // // TODO factor to base class and support scalarized dimensions
    auto shapeOp = cast<ShapeOp>(operation);
    auto shape = applyFunElementWise(shapeOp.getUB(), shapeOp.getLB(),
                                     std::minus<int64_t>());
    // auto strides = computeStrides(shape);
    // auto offset = computeOffset(shapeOp.getLB(), strides);
    // auto map =
    //     makeStridedLinearLayoutMap(strides, offset, assertOp.getContext());

    auto resultType =
        MemRefType::get(shape, fieldType.getElementType()); //, map, 0);

    rewriter.create<MemRefCastOp>(loc, operands[0], resultType);

    // rewriter.replaceOp(assertOp, {resultCast});

    rewriter.eraseOp(assertOp);

    return success();
  }
};

class LoadOpLowering : public StencilOpToStdPattern<stencil::LoadOp> {
public:
  using StencilOpToStdPattern<stencil::LoadOp>::StencilOpToStdPattern;

  LogicalResult
  matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = operation->getLoc();
    auto loadOp = cast<stencil::LoadOp>(operation);

    // Get the memref cast operation
    // TODO put in base class
    MemRefCastOp castOp;
    for (auto user : operands[0].getUsers()) {
      if (castOp = dyn_cast<MemRefCastOp>(user))
        break;
    }
    if (!castOp)
      return failure();

    // Get the assert operation
    // TODO put in base class
    stencil::AssertOp assertOp;
    for (auto user : loadOp.field().getUsers()) {
      if (assertOp = dyn_cast<stencil::AssertOp>(user)) 
        break;
    }
    if (!assertOp)
      return failure();
    
    
    auto inputType = castOp.getResult().getType().cast<MemRefType>();

    auto shapeOp = cast<ShapeOp>(operation);
    auto shape = applyFunElementWise(shapeOp.getUB(), shapeOp.getLB(),
                                     std::minus<int64_t>());
    auto strides = computeStrides(inputType.getShape());

    // TODO get assert operation!
    auto offset = computeOffset(applyFunElementWise(shapeOp.getLB(), cast<ShapeOp>(assertOp.getOperation()).getLB(), std::minus<int64_t>()), strides);
    auto map =
        makeStridedLinearLayoutMap(strides, offset, loadOp.getContext());

    auto resultType =
        MemRefType::get(shape, inputType.getElementType(), map, 0);

    auto subViewOp =
        rewriter.create<SubViewOp>(loc, resultType, castOp.getResult());
    rewriter.replaceOp(operation, subViewOp.getResult());

    return success();
  }
};

// class AssertOpLowering : public ConversionPattern {
// public:
//   explicit AssertOpLowering(MLIRContext *context)
//       : ConversionPattern(stencil::AssertOp::getOperationName(), 1, context)
//       {}

//   LogicalResult
//   matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
//                   ConversionPatternRewriter &rewriter) const override {
//     auto assertOp = cast<stencil::AssertOp>(operation);

//     // Verify the field has been converted
//     if (!assertOp.field().getType().isa<MemRefType>())
//       return failure();

//     // TODO refactor
//     // Verify the assert op has lower bound zero
//     // if (!isZero(assertOp.getLB())) {
//     //   assertOp.emitOpError("expected zero lower bound");
//     //   return failure();
//     // }

//     // Check if the field is large enough
//     // auto verifyBounds = [&](const Index &lb,
//     //                         const Index &ub) {
//     //   if (llvm::any_of(llvm::zip(lb, assertOp.getLB()),
//     //                    [](std::tuple<int64_t, int64_t> x) {
//     //                      return std::get<0>(x) < std::get<1>(x) &&
//     //                             std::get<0>(x) !=
//     stencil::kIgnoreDimension;
//     //                    }) ||
//     //       llvm::any_of(llvm::zip(ub, assertOp.getUB()),
//     //                    [](std::tuple<int64_t, int64_t> x) {
//     //                      return std::get<0>(x) > std::get<1>(x) &&
//     //                             std::get<1>(x) !=
//     stencil::kIgnoreDimension;
//     //                    }))
//     //     return false;
//     //   return true;
//     // };
//     // for (OpOperand &use : assertOp.field().getUses()) {
//     //   if (auto storeOp = dyn_cast<stencil::StoreOp>(use.getOwner())) {
//     //     if (llvm::is_contained(storeOp.getLB(),
//     stencil::kIgnoreDimension)) {
//     //       storeOp.emitOpError("field expected to have full
//     dimensionality");
//     //       return failure();
//     //     }
//     //     if (!verifyBounds(storeOp.getLB(), storeOp.getUB())) {
//     //       storeOp.emitOpError("field bounds not large enough");
//     //       return failure();
//     //     }
//     //   }
//     //   if (auto loadOp = dyn_cast<stencil::LoadOp>(use.getOwner())) {
//     //     if (loadOp.lb().hasValue() && loadOp.ub().hasValue())
//     //       if (!verifyBounds(loadOp.getLB(), loadOp.getUB())) {
//     //         loadOp.emitOpError("field bounds not large enough");
//     //         return failure();
//     //       }
//     //   }
//     // }

//     rewriter.eraseOp(operation);
//     return success();
//   }
// };

// class LoadOpLowering : public ConversionPattern {
// public:
//   explicit LoadOpLowering(MLIRContext *context)
//       : ConversionPattern(stencil::LoadOp::getOperationName(), 1, context) {}

//   LogicalResult
//   matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
//                   ConversionPatternRewriter &rewriter) const override {
//     // Replace the load operation with a subtemp op
//     auto loc = operation->getLoc();
//     auto loadOp = cast<stencil::LoadOp>(operation);

//     // TODO refactor
//     // // Verify the field has been converted
//     // if (!loadOp.field().getType().isa<MemRefType>())
//     //   return failure();

//     // // Compute the replacement types
//     // auto inputType = loadOp.field().getType().cast<MemRefType>();
//     // auto shape = computeShape(loadOp.getLB(), loadOp.getUB());
//     // auto strides = computeStrides(inputType.getShape());
//     // assert(shape.size() == inputType.getRank() &&
//     //        strides.size() == inputType.getRank() &&
//     //        "expected input field shape and strides to have the same
//     rank");
//     // auto outputType =
//     //     computeMemRefType(inputType.getElementType(), shape, strides,
//     //                       filterIgnoredDimensions(loadOp.getLB()),
//     rewriter);

//     // // Replace the load
//     // auto subViewOp = rewriter.create<SubViewOp>(loc, outputType,
//     operands[0]);
//     // operation->getResult(0).replaceAllUsesWith(subViewOp.getResult());
//     // rewriter.eraseOp(operation);
//     return success();
//   }
// };

// class ReturnOpLowering : public ConversionPattern {
// public:
//   explicit ReturnOpLowering(MLIRContext *context)
//       : ConversionPattern(stencil::ReturnOp::getOperationName(), 1, context)
//       {}

//   LogicalResult
//   matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
//                   ConversionPatternRewriter &rewriter) const override {
//     auto loc = operation->getLoc();
//     auto returnOp = cast<stencil::ReturnOp>(operation);

//     // Get the parallel loop
//     if (!isa<loop::ParallelOp>(operation->getParentOp()))
//       return failure();
//     auto loop = cast<loop::ParallelOp>(operation->getParentOp());
//     SmallVector<Value, 3> loopIVs(loop.getNumLoops());
//     llvm::transform(loop.getInductionVars(), loopIVs.begin(),
//                     [](Value blockArg) { return blockArg; });

//     // Get temporary buffers
//     SmallVector<Operation *, 10> allocOps;
//     Operation *currentOp = loop.getOperation();
//     // Skip the loop constants
//     while (isa<ConstantOp>(currentOp->getPrevNode())) {
//       currentOp = currentOp->getPrevNode();
//       assert(currentOp && "failed to find allocation for results");
//     }
//     // Compute the number of result temps
//     unsigned numResults =
//         returnOp.getNumOperands() / returnOp.getUnrollFactor();
//     assert(returnOp.getNumOperands() % returnOp.getUnrollFactor() == 0 &&
//            "expected number of operands to be a multiple of the unroll
//            factor");
//     for (unsigned i = 0, e = numResults; i != e; ++i) {
//       currentOp = currentOp->getPrevNode();
//       allocOps.push_back(currentOp);
//       assert(dyn_cast<AllocOp>(currentOp) &&
//              "failed to find allocation for results");
//     }
//     SmallVector<Value, 10> allocVals(allocOps.size());
//     llvm::transform(llvm::reverse(allocOps), allocVals.begin(),
//                     [](Operation *allocOp) { return allocOp->getResult(0);
//                     });

//     // Replace the return op by store ops
//     auto expr = rewriter.getAffineDimExpr(0) + rewriter.getAffineDimExpr(1);
//     auto map = AffineMap::get(2, 0, expr);
//     for (unsigned i = 0, e = numResults; i != e; ++i) {
//       for (unsigned j = 0, e = returnOp.getUnrollFactor(); j != e; ++j) {
//         rewriter.setInsertionPoint(returnOp);
//         unsigned operandIdx = i * returnOp.getUnrollFactor() + j;
//         auto definingOp = returnOp.getOperand(operandIdx).getDefiningOp();
//         if (definingOp && returnOp.getParentOp() ==
//         definingOp->getParentOp())
//           rewriter.setInsertionPointAfter(definingOp);
//         // Add the unrolling offset to the loop ivs
//         SmallVector<Value, 3> storeOffset = loopIVs;
//         if (j > 0) {
//           auto unroll = returnOp.getUnroll();
//           assert(llvm::count(unroll, 1) == unroll.size() - 1 &&
//                  "expected a single non-zero entry");
//           auto it = llvm::find_if(unroll, [&](int64_t x) {
//             return x == returnOp.getUnrollFactor();
//           });
//           auto unrollDim = std::distance(unroll.begin(), it);
//           auto constantOp = rewriter.create<ConstantIndexOp>(loc, j);
//           ValueRange params = {loopIVs[unrollDim], constantOp.getResult()};
//           auto affineApplyOp = rewriter.create<AffineApplyOp>(loc, map,
//           params); storeOffset[unrollDim] = affineApplyOp.getResult();
//         }
//         rewriter.create<mlir::StoreOp>(loc, returnOp.getOperand(operandIdx),
//                                  allocVals[i], storeOffset);
//       }
//     }
//     rewriter.eraseOp(operation);
//     return success();
//   }
// };

// class ApplyOpLowering : public ConversionPattern {
// public:
//   explicit ApplyOpLowering(MLIRContext *context)
//       : ConversionPattern(stencil::ApplyOp::getOperationName(), 1, context)
//       {}

//   LogicalResult
//   matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
//                   ConversionPatternRewriter &rewriter) const override {
//     auto loc = operation->getLoc();
//     auto applyOp = cast<stencil::ApplyOp>(operation);

//     // TODO refactor
//     // // Verify the arguments have been converted
//     // if (llvm::any_of(
//     //         llvm::zip(applyOp.getBody()->getArguments(),
//     applyOp.getOperands()),
//     //         [](std::tuple<Value, Value> x) {
//     //           return std::get<0>(x).getType().isa<stencil::TempType>() &&
//     //                  !std::get<1>(x).getType().isa<MemRefType>();
//     //         })) {
//     //   return failure();
//     // }

//     // // Verify the the lower bound is zero
//     // if (!isZero(applyOp.getLB())) {
//     //   applyOp.emitOpError("expected zero lower bound");
//     //   return failure();
//     // }

//     // // Allocate and deallocate storage for every output
//     // for (unsigned i = 0, e = applyOp.getNumResults(); i != e; ++i) {
//     //   Type elementType = stencil::getElementType(applyOp.getResult(i));
//     //   auto strides = computeStrides(applyOp.getUB());
//     //   auto allocType = computeMemRefType(elementType, applyOp.getUB(),
//     strides,
//     //                                      {}, rewriter);

//     //   auto allocOp = rewriter.create<AllocOp>(loc, allocType);
//     //   applyOp.getResult(i).replaceAllUsesWith(allocOp.getResult());
//     //   auto returnOp = allocOp.getParentRegion()->back().getTerminator();
//     //   rewriter.setInsertionPoint(returnOp);
//     //   rewriter.create<DeallocOp>(loc, allocOp.getResult());
//     //   rewriter.setInsertionPointAfter(allocOp);
//     // }

//     // // Generate the apply loop nest
//     // auto upper = applyOp.getUB();
//     // assert(upper.size() >= 1 && "expected bounds to at least one
//     dimension");
//     // auto zero = rewriter.create<ConstantIndexOp>(loc, 0);
//     // auto one = rewriter.create<ConstantIndexOp>(loc, 1);
//     // SmallVector<Value, 3> lb;
//     // SmallVector<Value, 3> ub;
//     // SmallVector<Value, 3> steps;
//     // for (size_t i = 0, e = upper.size(); i != e; ++i) {
//     //   lb.push_back(zero);
//     //   ub.push_back(rewriter.create<ConstantIndexOp>(loc,
//     upper.begin()[i]));
//     //   steps.push_back(one);
//     // }

//     // // Adjust the steps to account for the loop unrolling
//     // auto returnOp =
//     cast<stencil::ReturnOp>(applyOp.getBody()->getTerminator());
//     // assert(!returnOp.unroll().hasValue() ||
//     //        steps.size() == returnOp.unroll().getValue().size() &&
//     //            "expected unroll attribute to have loop bound size");
//     // if (returnOp.unroll().hasValue()) {
//     //   auto unroll = returnOp.getUnroll();
//     //   for (size_t i = 0, e = steps.size(); i != e; ++i) {
//     //     if (unroll[i] != 1) {
//     //       assert(upper.begin()[i] % unroll[i] == 0 &&
//     //              "expected loop length to be a multiple of the unroll
//     factor");
//     //       steps[i] = rewriter.create<ConstantIndexOp>(loc, unroll[i]);
//     //     }
//     //   }
//     // }

//     // // Introduce the parallel loop and copy the body of the apply op
//     // auto loop = rewriter.create<loop::ParallelOp>(loc, lb, ub, steps);
//     // for (size_t i = 0, e = applyOp.operands().size(); i < e; ++i) {
//     //   applyOp.getBody()->getArgument(i).replaceAllUsesWith(
//     //       applyOp.getOperand(i));
//     // }
//     // loop.getBody()->getOperations().splice(
//     //     loop.getBody()->getOperations().begin(),
//     //     applyOp.getBody()->getOperations());

//     // // Erase the actual apply op
//     // rewriter.eraseOp(applyOp);

//     return success();
//   }
// }; // namespace

// class AccessOpLowering : public ConversionPattern {
// public:
//   explicit AccessOpLowering(MLIRContext *context)
//       : ConversionPattern(stencil::AccessOp::getOperationName(), 1, context)
//       {}

//   LogicalResult
//   matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
//                   ConversionPatternRewriter &rewriter) const override {
//     auto loc = operation->getLoc();
//     auto accessOp = cast<stencil::AccessOp>(operation);

//     // Check the temp is a memref type
//     if (!accessOp.temp().getType().isa<MemRefType>())
//       return failure();

//     // Get the parallel loop
//     auto loopOp = operation->getParentOfType<loop::ParallelOp>();
//     if (!loopOp)
//       return failure();
//     assert(loopOp.getNumLoops() == accessOp.getOffset().size() &&
//            "expected loop nest and access offset to have the same size");
//     SmallVector<Value, 3> loopIVs(loopOp.getNumLoops());
//     llvm::transform(loopOp.getInductionVars(), loopIVs.begin(),
//                     [](Value arg) { return arg; });

//     // Compute the access offsets
//     auto expr = rewriter.getAffineDimExpr(0) + rewriter.getAffineDimExpr(1);
//     auto map = AffineMap::get(2, 0, expr);
//     auto offset = accessOp.getOffset();
//     SmallVector<Value, 3> loadOffset;
//     for (size_t i = 0, e = offset.size(); i != e; ++i) {
//       auto constantOp = rewriter.create<ConstantIndexOp>(loc, offset[i]);
//       ValueRange params = {loopIVs[i], constantOp.getResult()};
//       auto affineApplyOp = rewriter.create<AffineApplyOp>(loc, map, params);
//       loadOffset.push_back(affineApplyOp.getResult());
//     }
//     assert(loadOffset.size() ==
//                accessOp.temp().getType().cast<MemRefType>().getRank() &&
//            "expected load offset size to match memref rank");

//     // Replace the access op by a load op
//     rewriter.replaceOpWithNewOp<mlir::LoadOp>(operation, accessOp.temp(),
//     loadOffset); return success();
//   }
// };

// class StoreOpLowering : public ConversionPattern {
// public:
//   explicit StoreOpLowering(MLIRContext *context)
//       : ConversionPattern(stencil::StoreOp::getOperationName(), 1, context)
//       {}

//   LogicalResult
//   matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
//                   ConversionPatternRewriter &rewriter) const override {
//     auto loc = operation->getLoc();
//     auto storeOp = cast<stencil::StoreOp>(operation);

//     // TODO refactor
//     // // Verify the field has been converted
//     // if (!(storeOp.field().getType().isa<MemRefType>() &&
//     //       storeOp.temp().getType().isa<MemRefType>()))
//     //   return failure();

//     // // Compute the replacement types
//     // auto inputType = storeOp.field().getType().cast<MemRefType>();
//     // assert(storeOp.getLB().size() == inputType.getRank() &&
//     //        "expected lower bounds and memref to have the same rank");
//     // auto shape = computeShape(storeOp.getLB(), storeOp.getUB());
//     // auto strides = computeStrides(inputType.getShape());
//     // auto outputType = computeMemRefType(inputType.getElementType(), shape,
//     //                                     strides, storeOp.getLB(),
//     rewriter);

//     // // Remove allocation and deallocation and insert subtemp op
//     // auto allocOp = storeOp.temp().getDefiningOp();
//     // rewriter.setInsertionPoint(allocOp);
//     // auto subViewOp =
//     //     rewriter.create<SubViewOp>(loc, outputType, storeOp.field());
//     // allocOp->getResult(0).replaceAllUsesWith(subViewOp.getResult());
//     // rewriter.eraseOp(allocOp);
//     // for (auto &use : storeOp.temp().getUses()) {
//     //   if (auto deallocOp = dyn_cast<DeallocOp>(use.getOwner()))
//     //     rewriter.eraseOp(deallocOp);
//     // }
//     // rewriter.eraseOp(operation);
//     return success();
//   }
// };

//===----------------------------------------------------------------------===//
// Conversion Target
//===----------------------------------------------------------------------===//

class StencilToStdTarget : public ConversionTarget {
public:
  explicit StencilToStdTarget(MLIRContext &context)
      : ConversionTarget(context) {}

  bool isDynamicallyLegal(Operation *op) const override {
    if (auto funcOp = dyn_cast<FuncOp>(op)) {
      return !funcOp.getAttr(
                 stencil::StencilDialect::getStencilProgramAttrName()) &&
             !funcOp.getAttr(
                 stencil::StencilDialect::getStencilFunctionAttrName());
    }
    return true;
  }
};

//===----------------------------------------------------------------------===//
// Rewriting Pass
//===----------------------------------------------------------------------===//

struct StencilToStandardPass
    : public StencilToStandardPassBase<StencilToStandardPass> {
  void runOnOperation() override;
};

void StencilToStandardPass::runOnOperation() {
  OwningRewritePatternList patterns;
  auto module = getOperation();

  StencilTypeConverter typeConverter;

  populateStencilToStdConversionPatterns(module.getContext(), typeConverter,
                                         patterns);

  StencilToStdTarget target(*(module.getContext()));
  target.addLegalDialect<AffineDialect>();
  target.addLegalDialect<StandardOpsDialect>();
  target.addLegalDialect<loop::LoopOpsDialect>();
  target.addDynamicallyLegalOp<FuncOp>();
  target.addLegalOp<ModuleOp, ModuleTerminatorOp>();

  if (failed(applyFullConversion(module, target, patterns, &typeConverter))) {
    signalPassFailure();
  }
}

} // namespace

namespace mlir {
namespace stencil {

// Implementation of the Stencil Type converter
StencilTypeConverter::StencilTypeConverter() {
  addConversion([&](FieldType type) { return convertFieldType(type); });
  addConversion([&](Type type) -> Optional<Type> {
    if (auto fieldType = type.dyn_cast<FieldType>())
      return llvm::None;
    return type;
  });
}

Type StencilTypeConverter::convertFieldType(FieldType type) {
  auto elementType = type.getElementType();
  SmallVector<int64_t, kIndexSize> shape, strides;
  for (auto size : type.getShape()) {
    assert(GridType::isScalar(size) ||
           GridType::isDynamic(size) &&
               "expected fields to have a dynamic shape");
    if (GridType::isDynamic(size)) {
      shape.push_back(ShapedType::kDynamicSize);
      strides.push_back(ShapedType::kDynamicStrideOrOffset);
    }
  }
  // auto affineMap = makeStridedLinearLayoutMap(
  //     strides, ShapedType::kDynamicStrideOrOffset, type.getContext());
  return MemRefType::get(shape, elementType); //, affineMap, 0);
}

// Implementation of the stencil to standard pattern
StencilToStdPattern::StencilToStdPattern(StringRef rootOpName,
                                         MLIRContext *context,
                                         StencilTypeConverter &typeConverter_,
                                         PatternBenefit benefit)
    : ConversionPattern(rootOpName, benefit, context),
      typeConverter(typeConverter_) {}

void populateStencilToStdConversionPatterns(
    mlir::MLIRContext *ctx, StencilTypeConverter &typeConveter,
    mlir::OwningRewritePatternList &patterns) {

  // patterns
  //     .insert<FuncOpLowering, AssertOpLowering, LoadOpLowering,
  //     ApplyOpLowering,
  //             AccessOpLowering, StoreOpLowering, ReturnOpLowering>(ctx);

  patterns.insert<FuncOpLowering, AssertOpLowering, LoadOpLowering>(
      ctx, typeConveter);
}

} // namespace stencil
} // namespace mlir

std::unique_ptr<Pass> mlir::createConvertStencilToStandardPass() {
  return std::make_unique<StencilToStandardPass>();
}
