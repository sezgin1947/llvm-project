//===- TosaFolders.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Fold TOSA operations
//
//===----------------------------------------------------------------------===//

#include <functional>

#include "mlir/Dialect/Tosa/IR/TosaOps.h"
#include "mlir/Dialect/Tosa/Transforms/Passes.h"
#include "mlir/Dialect/Utils/IndexingUtils.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/FloatingPointMode.h"
#include "llvm/ADT/SmallVector.h"

using namespace mlir;
using namespace mlir::tosa;

namespace {

/// Type that represents tensor dimensions.
using DimensionType = ArrayRef<int64_t>;

/// Type for tensor offsets.
using OffsetType = size_t;

/// Rounding mode to be used on floating point operations that require rounding.
static constexpr llvm::RoundingMode tosaRoundingMode =
    llvm::APFloat::rmNearestTiesToEven;

OffsetType indexToOffset(DimensionType shape, DimensionType index) {
  OffsetType offset = 0;
  for (size_t i = 0; i < shape.size(); i++) {
    offset = offset * shape[i] + index[i];
  }
  return offset;
}

SmallVector<int64_t> offsetToIndex(DimensionType shape,
                                               OffsetType offset) {
  auto rank = shape.size();
  // The rank of the index will be equal to the rank of the shape
  SmallVector<int64_t> resultIndex;
  resultIndex.reserve(rank);
  // Compute all the index values from the last to the first one, reverse the
  // vector afterwards as there is no convenient push_front.
  for (int32_t i = rank - 1; i >= 0; i--) {
    resultIndex.push_back(offset % shape[i]);
    offset /= shape[i];
  }
  std::reverse(resultIndex.begin(), resultIndex.end());
  return resultIndex;
}

SmallVector<int64_t>
getBroadcastedIndex(DimensionType desiredShape,
                                DimensionType toBeBroadcastedShape,
                                DimensionType index) {
  SmallVector<int64_t> broadCasted;
  broadCasted.reserve(desiredShape.size());
  for (size_t i = 0; i < desiredShape.size(); i++) {
    auto toInsert = 0;
    if (toBeBroadcastedShape[i] == desiredShape[i]) {
      toInsert = index[i];
    }
    broadCasted.push_back(toInsert);
  }
  return broadCasted;
}

OffsetType getBroadcastedOffset(DimensionType desiredShape,
                                            DimensionType toBeBroadcastedShape,
                                            OffsetType offset) {
  // Simply return the offset if the shapes are equal.
  if (desiredShape.equals(toBeBroadcastedShape)) {
    return offset;
  }
  auto indexInTarget = offsetToIndex(desiredShape, offset);
  auto indexBroadcasted =
      getBroadcastedIndex(desiredShape, toBeBroadcastedShape, indexInTarget);
  return indexToOffset(toBeBroadcastedShape, indexBroadcasted);
}


/// Apply the given transformation \p toApply to every element of the tensor to
/// be transformed \p toTransform.
///
/// Elements of \p toTransform are extracted as \p SrcValueType.
///
/// \returns A tensor with the same size as \p toTransform, containing
/// \p TargetValueType values of type \p TargetType.
template <class SrcValType, class TargetValType, class TargetType>
DenseElementsAttr applyElementWise(
    const DenseElementsAttr &toTransform,
    const std::function<TargetValType(const SrcValType &, TargetType)> &toApply,
    TargetType targetType) {
  SmallVector<TargetValType> transformedValues;
  // We already know the amount of values we will insert, reserve space for
  // all of them to avoid dynamic resizing
  transformedValues.reserve(toTransform.getNumElements());
  for (auto val : toTransform.getValues<SrcValType>()) {
    auto transformedVal = toApply(val, targetType);
    transformedValues.push_back(transformedVal);
  }

  // Make sure that the output tensor has the expected output type
  auto inShape = toTransform.getType();
  auto outTy = inShape.cloneWith({}, targetType);

  return DenseElementsAttr::get(outTy, transformedValues);
}

template DenseElementsAttr applyElementWise<APFloat, APFloat, FloatType>(
    const DenseElementsAttr &toTransform,
    const std::function<APFloat(const APFloat &, FloatType)> &toApply,
    FloatType targetType);

/// Function that checks if the type contained in \p toCheck is float.
LogicalResult notifyIfNotFloat(TypedValue<TensorType> toCheck, TosaOp location,
                               PatternRewriter &rewriter) {
  if (isa<FloatType>(toCheck.getType().getElementType())) {
    return success();
  }
  return rewriter.notifyMatchFailure(location,
                                     "Unexpected input tensor type: the "
                                     "TOSA spec only allows floats");
}

template <class ElementType, class ResultType>
DenseElementsAttr applyElementWise(
    const DenseElementsAttr &first, const DenseElementsAttr &second,
    TensorType targetType,
    const std::function<ResultType(const ElementType &, const ElementType &)>
        &toApply) {
  // Make sure to use the correct values in case broadcasting is required
  SmallVector<ResultType> transformedValues;
  // We already know the amount of values we will insert, reserve space for
  // all of them to avoid dynamic resizing
  auto targetSize = 1;
  auto targetShape = targetType.getShape();
  for (const auto &dimSize : targetShape) {
    targetSize *= dimSize;
  }
  transformedValues.reserve(targetSize);

  // Apply the given function to each pair of values from the input tensors.
  // Make sure to broadcast the offsets properly.
  auto firstIt = first.getValues<ElementType>();
  auto firstShape = first.getType().getShape();
  auto secondIt = second.getValues<ElementType>();
  auto secondShape = second.getType().getShape();
  for (auto offset = 0; offset < targetSize; offset++) {
    OffsetType offsetInTargetFirst =
        getBroadcastedOffset(targetShape, firstShape, offset);
    OffsetType offsetInTargetSecond =
        getBroadcastedOffset(targetShape, secondShape, offset);
    auto res =
        toApply(firstIt[offsetInTargetFirst], secondIt[offsetInTargetSecond]);
    transformedValues.push_back(res);
  }

  // Generate a tensor with the computed values.
  auto newTensor = DenseElementsAttr::get(targetType, transformedValues);
  return newTensor;
}

/// Function that checks if \p toCheck is a dense TOSA constant tensor.
LogicalResult notifyIfNoTosaDenseConstantTensor(Value toCheck,
                                                TosaOp location,
                                                PatternRewriter &rewriter) {
  // Check whether the tensor is constant and dense
  // TODO We currently ensure the tensor is dense by using the correct type for
  // the bind_value, however we do not actually need this value. It would be
  // nicer to only have a check here.
  DenseElementsAttr tmp;
  if (!matchPattern(toCheck, m_Constant(&tmp))) {
    return rewriter.notifyMatchFailure(location,
                                       "Non-const or non-dense input tensor");
  }

  // Make sure it actually is a TOSA constant (the match allows for other
  // constants as well)
  if (isa<ConstOp>(toCheck.getDefiningOp())) {
    return success();
  }

  return rewriter.notifyMatchFailure(location,
                                     "The reciprocal can only be folded if "
                                     "it operates on a TOSA constant");
}

template <typename BaseType>
DenseElementsAttr transposeType(ElementsAttr attr, ShapedType inputType,
                                ShapedType outputType,
                                llvm::ArrayRef<int64_t> permValues) {
  if (inputType.getNumElements() == 0)
    return DenseElementsAttr::get(outputType, llvm::ArrayRef<BaseType>{});

  auto attrValues = attr.getValues<BaseType>();
  auto inputShape = inputType.getShape();

  // The inverted permutation map and strides of the output are used to compute
  // the contribution of a given dimension to the destination linear index in
  // an order-independent way.
  auto outputStrides = computeStrides(outputType.getShape());
  auto invertedPermValues = invertPermutationVector(permValues);

  auto initialValue = *std::begin(attrValues);
  SmallVector<BaseType> outputValues(inputType.getNumElements(), initialValue);

  for (const auto &it : llvm::enumerate(attrValues)) {
    auto srcLinearIndex = it.index();

    uint64_t dstLinearIndex = 0;
    for (int64_t dim = inputShape.size() - 1; dim >= 0; --dim) {
      // Compute the index into the current dimension of the source vector.
      auto sourceIndexForDim = srcLinearIndex % inputShape[dim];
      srcLinearIndex /= inputShape[dim];

      // Add the contribution of the current dimension to the output using the
      // permutation map.
      dstLinearIndex +=
          outputStrides[invertedPermValues[dim]] * sourceIndexForDim;
    }

    outputValues[dstLinearIndex] = it.value();
  }

  return DenseElementsAttr::get(outputType,
                                llvm::ArrayRef<BaseType>(outputValues));
}

// A type specialized transposition of an ElementsAttr.
// This implementation tries to operate on the underlying data in its raw
// representation when possible to avoid allocating a large number of Attribute
// objects.
DenseElementsAttr transpose(ElementsAttr attr, ShapedType inputType,
                            ShapedType outputType,
                            llvm::ArrayRef<int64_t> permValues) {
  auto baseType = inputType.getElementType();

  // Handle possible integer types
  if (auto intType = dyn_cast<IntegerType>(baseType)) {
    switch (intType.getWidth()) {
    case 1:
      return transposeType<bool>(attr, inputType, outputType, permValues);
    case 8:
      return transposeType<int8_t>(attr, inputType, outputType, permValues);
    case 16:
      return transposeType<int16_t>(attr, inputType, outputType, permValues);
    case 32:
      return transposeType<int32_t>(attr, inputType, outputType, permValues);
    case 64:
      return transposeType<int64_t>(attr, inputType, outputType, permValues);
    default:
      return transposeType<APInt>(attr, inputType, outputType, permValues);
    }
  }

  // Handle possible float types
  if (baseType.isF32()) {
    return transposeType<float>(attr, inputType, outputType, permValues);
  }

  return transposeType<APFloat>(attr, inputType, outputType, permValues);
}

template<typename TosaOp>
struct TosaFoldConstantBase: public OpRewritePattern<TosaOp> {
  TosaFoldConstantBase(MLIRContext* ctxt, bool foldSplatOrSingleUseOnly) : OpRewritePattern<TosaOp>(ctxt), foldSplatOrSingleUseOnly(foldSplatOrSingleUseOnly) {}

  bool foldSplatOrSingleUseOnly;

  /// Heuristic to decide when to replace a unary operation on a constant with the
  /// folded value.
  /// Folding operations on constants can lead to an increased memory usage
  /// whenever the input cannot be replaced but a new constant is inserted. Hence,
  /// this will currently only suggest folding when the memory impact is
  /// negligible.
  /// Takes the \p unaryOp and the constant input \p values.
  /// \returns Whether folding should be applied.
  bool constantUnaryOpShouldBeFolded(TosaOp unaryOp, DenseElementsAttr values) const {
    if (!foldSplatOrSingleUseOnly)
      return true;
    assert(unaryOp->getNumOperands() == 1);
    auto inputOp = unaryOp->getOperand(0);

    // If the input is a splat, we don't care for the number of users
    if (isa<SplatElementsAttr>(values)) {
      return true;
    }

    // If this is the only use of the tensor it should be replaced as no
    // additional memory is required
    return inputOp.hasOneUse();
  }

  bool constantBinaryOpShouldBeFolded(
      TosaOp binaryOp, DenseElementsAttr valuesFirst,
      DenseElementsAttr valuesSecond) const {
    if (!foldSplatOrSingleUseOnly)
      return true;
    assert(binaryOp->getNumOperands() == 2);
    auto firstOp = binaryOp->getOperand(0);
    auto secondOp = binaryOp->getOperand(1);

    // If both tensors are splat, we don't care for the number of users
    if (isa<SplatElementsAttr>(valuesFirst) &&
        isa<SplatElementsAttr>(valuesSecond)) {
      return true;
    }

    // If this is the only use of one of the tensors, it will be replaced an no
    // additional memory is required.
    if (firstOp.hasOneUse() || secondOp.hasOneUse()) {
      return true;
    }

    // Fold it both inputs are equal and those are the only uses. Don't fold
    // otherwise.
    auto numUsers =
        std::distance(firstOp.getUses().begin(), firstOp.getUses().end());
    return firstOp == secondOp && numUsers == 2;
  }
};

template <typename BaseClass, typename TosaOp>
struct TosaFoldConstantUnaryElementwise : public TosaFoldConstantBase<TosaOp> {
  using TosaFoldConstantBase<TosaOp>::TosaFoldConstantBase;

  LogicalResult matchAndRewrite(TosaOp op,
                                PatternRewriter &rewriter) const override {
    auto inputTensor = op.getOperand();
    // Check that we can apply folding
    auto preCondCheck =
        notifyIfNoTosaDenseConstantTensor(inputTensor, op, rewriter);
    if (failed(preCondCheck)) {
      return preCondCheck;
    }

    // Extract the tensor values
    DenseElementsAttr inputValues;
    matchPattern(inputTensor, m_Constant(&inputValues));

    // Check whether this should be folded.
    if (!TosaFoldConstantBase<TosaOp>::constantUnaryOpShouldBeFolded(
            op, inputValues)) {
      return rewriter.notifyMatchFailure(
          op, "Currently, unary ops will only be folded if the input "
              "tensor has a single user");
    }

    TensorType opType = dyn_cast<TensorType>(op.getType());
    if (opType == nullptr ||
        !static_cast<const BaseClass *>(this)->isSupportedElementType(
            opType.getElementType())) {
      return rewriter.notifyMatchFailure(op, "Type is not supported.");
    }

    DenseElementsAttr newTensor = static_cast<const BaseClass *>(this)->compute(
        inputValues, rewriter, op);
    if (!newTensor) {
      return rewriter.notifyMatchFailure(op,
                                         "Type or values cannot be folded.");
    }
    rewriter.replaceOpWithNewOp<ConstOp>(op, newTensor.getType(), newTensor);
    return success();
  }

  DenseElementsAttr compute(DenseElementsAttr values, PatternRewriter &rewriter,
                            TosaOp op) const {
    if (isa<IntegerType>(values.getElementType()))
      return static_cast<const BaseClass *>(this)->computeInteger(values,
                                                                  rewriter, op);

    assert(isa<FloatType>(values.getElementType()));
    return static_cast<const BaseClass *>(this)->computeFloat(values, rewriter,
                                                              op);
  }

  /// Called when the values.getElementType() is IntegerType.
  DenseElementsAttr computeInteger(DenseElementsAttr values,
                                   PatternRewriter &rewriter, TosaOp op) const {
    return {};
  }

  /// Called when the values.getElementType() is FloatType.
  DenseElementsAttr computeFloat(DenseElementsAttr values,
                                 PatternRewriter &rewriter, TosaOp op) const {
    return {};
  }

  /// Return true if the \p elementType is supported by the folder.
  bool isSupportedElementType(Type type) const { return true; }
};

template<typename BaseClass, typename TosaOp>
struct TosaFoldConstantBinary : public TosaFoldConstantBase<TosaOp> {
  using TosaFoldConstantBase<TosaOp>::TosaFoldConstantBase;

  LogicalResult matchAndRewrite(TosaOp op,
                                PatternRewriter &rewriter) const override {
    auto leftOp = op.getOperand(0);
    auto rightOp = op.getOperand(1);

    auto lhsTensorType = dyn_cast<TensorType>(leftOp.getType());
    auto rhsTensorType = dyn_cast<TensorType>(rightOp.getType());
    if (!lhsTensorType || !rhsTensorType) {
      return rewriter.notifyMatchFailure(
          op, "Expected types to be tensors.");
    }

    auto lhsElemType = lhsTensorType.getElementType();
    auto rhsElemType = rhsTensorType.getElementType();
    if (lhsElemType != rhsElemType) {
      return rewriter.notifyMatchFailure(
          op, "Expected type of binary op arguments to match.");
    }

    // Check if both tensors are constant
    auto rhsIsConstantCheck =
        notifyIfNoTosaDenseConstantTensor(leftOp, op, rewriter);
    if (failed(rhsIsConstantCheck)) {
      return rhsIsConstantCheck;
    }
    auto lhsIsConstantCheck =
        notifyIfNoTosaDenseConstantTensor(rightOp, op, rewriter);
    if (failed(lhsIsConstantCheck)) {
      return lhsIsConstantCheck;
    }

    // Extract the tensor values
    DenseElementsAttr lhsValues;
    matchPattern(leftOp, m_Constant(&lhsValues));

    DenseElementsAttr rhsValues;
    matchPattern(rightOp, m_Constant(&rhsValues));

    if (!TosaFoldConstantBase<TosaOp>::constantBinaryOpShouldBeFolded(op, lhsValues, rhsValues)) {
      return rewriter.notifyMatchFailure(
          op, "Currently, binary ops will only be folded if this requires only "
                 "little additional memory usage.");
    }

    DenseElementsAttr newTensor = static_cast<const BaseClass *>(this)->compute(
        lhsValues, rhsValues, rewriter, op);
    if (!newTensor) {
        return rewriter.notifyMatchFailure(
          op, "Type or values cannot be folded.");
    }
    rewriter.replaceOpWithNewOp<ConstOp>(op, newTensor.getType(), newTensor);
    return success();
  }

  DenseElementsAttr compute(DenseElementsAttr lhsValues,
                            DenseElementsAttr rhsValues,
                            PatternRewriter &rewriter, TosaOp op) const {
    if (isa<IntegerType>(lhsValues.getElementType()))
        return static_cast<const BaseClass *>(this)->computeInteger(
            lhsValues, rhsValues, rewriter, op);

    assert(isa<FloatType>(lhsValues.getElementType()));
    return static_cast<const BaseClass *>(this)->computeFloat(
        lhsValues, rhsValues, rewriter, op);
  }

  /// Called when the lhsValues.getElementType() is IntegerType.
  DenseElementsAttr computeInteger(DenseElementsAttr lhsValues,
                                   DenseElementsAttr rhsValues,
                                   PatternRewriter &rewriter, TosaOp op) const {
    return {};
  }

  /// Called when the lhsValues.getElementType() is FloatType.
  DenseElementsAttr computeFloat(DenseElementsAttr lhsValues,
                                 DenseElementsAttr rhsValues,
                                 PatternRewriter &rewriter, TosaOp op) const {
    return {};
  }
};

struct TosaFoldConstantTranspose : public TosaFoldConstantBase<tosa::TransposeOp> {
  using TosaFoldConstantBase::TosaFoldConstantBase;

  LogicalResult matchAndRewrite(tosa::TransposeOp op,
                                PatternRewriter &rewriter) const override {
    auto outputType = cast<ShapedType>(op.getType());
    // TOSA supports quantized types.
    if (!outputType.getElementType().isIntOrIndexOrFloat())
      return failure();

    ElementsAttr inputValues;
    if (!matchPattern(op.getInput1(), m_Constant(&inputValues)))
      return failure();
    // Make sure the input is a constant that has a single user.
    if (!llvm::hasSingleElement(op.getInput1().getDefiningOp()->getUsers()))
      return failure();

    DenseIntElementsAttr permAttr;
    if (!matchPattern(op.getPerms(), m_Constant(&permAttr)))
      return failure();
    auto permValues = llvm::to_vector<6>(llvm::map_range(
        // TOSA allows both 32- and 64-bit integer tensors here.
        permAttr.getValues<APInt>(),
        [](const APInt &val) { return val.getSExtValue(); }));

    auto inputType = cast<ShapedType>(op.getInput1().getType());

    auto resultAttr = transpose(inputValues, inputType, outputType, permValues);
    rewriter.replaceOpWithNewOp<tosa::ConstOp>(op, outputType, resultAttr);
    return success();
  }
};

static APFloat computeReciprocal(const APFloat &floatVal, FloatType floatTy) {
  auto recipAttr = FloatAttr::get(floatTy, 1.0);
  APFloat recip = recipAttr.getValue();
  recip.divide(floatVal, tosaRoundingMode);
  return recip;
}

struct TosaFoldConstantReciprocal
    : public TosaFoldConstantUnaryElementwise<TosaFoldConstantReciprocal, ReciprocalOp> {
  using TosaFoldConstantUnaryElementwise<TosaFoldConstantReciprocal,
                              ReciprocalOp>::TosaFoldConstantUnaryElementwise;

  DenseElementsAttr computeFloat(DenseElementsAttr values,
                                 PatternRewriter &rewriter, TosaOp op) const {
    return applyElementWise<APFloat, APFloat, FloatType>(
        values, &computeReciprocal, cast<FloatType>(values.getElementType()));
  }
};

struct TosaFoldConstantRSQRT
    : public TosaFoldConstantUnaryElementwise<TosaFoldConstantRSQRT, RsqrtOp> {
  using TosaFoldConstantUnaryElementwise<TosaFoldConstantRSQRT,
                              RsqrtOp>::TosaFoldConstantUnaryElementwise;

  static APFloat computeRSQRT(const APFloat &apFloatVal, FloatType floatTy) {
    // The result for negative values (apart from zero) is always NaN
    if (apFloatVal.isNegative() && !apFloatVal.isNegZero()) {
      return APFloat::getNaN(apFloatVal.getSemantics());
    }

    // Compute the square root (APFloat unfortunately does not provide this
    // function, such that we need to unpack here)
    auto floatVal = apFloatVal.convertToFloat();
    auto sqrtVal = std::sqrt(floatVal);
    APFloat apSqrtVal(sqrtVal);

    // Compute the reciprocal
    return computeReciprocal(apSqrtVal, floatTy);
  }

  DenseElementsAttr computeFloat(DenseElementsAttr values,
                                 PatternRewriter &rewriter, TosaOp op) const {
    return applyElementWise<APFloat, APFloat, FloatType>(
        values, &computeRSQRT, cast<FloatType>(values.getElementType()));
  }

  bool isSupportedElementType(Type type) const {
    return type.isBF16() || type.isF16() || type.isF32();
  }
};

struct TosaFoldConstantLogicalNot
    : public TosaFoldConstantUnaryElementwise<TosaFoldConstantLogicalNot,
                                              LogicalNotOp> {
  using TosaFoldConstantUnaryElementwise<
      TosaFoldConstantLogicalNot,
      LogicalNotOp>::TosaFoldConstantUnaryElementwise;

  DenseElementsAttr computeInteger(DenseElementsAttr values,
                                   PatternRewriter &rewriter, TosaOp op) const {
    return applyElementWise<APInt, APInt, IntegerType>(
        values,
        [](const APInt &val, IntegerType) {
          return APInt(1, !val.getBoolValue());
        },
        cast<IntegerType>(values.getElementType()));
  }
};

struct TosaFoldConstantPow
    : public TosaFoldConstantBinary<TosaFoldConstantPow, PowOp> {
  using TosaFoldConstantBinary<TosaFoldConstantPow,
                               PowOp>::TosaFoldConstantBinary;

  static APFloat computePower(const APFloat &base, const APFloat &exp) {
    // Propagate NaN
    if (base.isNaN() || exp.isNaN()) {
      return APFloat::getNaN(base.getSemantics());
    }
    // TOSA defines 0.0**0.0 as NaN
    if (base.isZero() && exp.isZero()) {
      return APFloat::getNaN(base.getSemantics());
    }
    // In case the value is negative, the exponent needs to be an integer
    if (base.isNegative() && !base.isZero()) {
      if (!exp.isInteger()) {
        return APFloat::getNaN(base.getSemantics());
      }
    }

    // Actually compute base**exp. Special cases for [-]infinity and [-]0 are
    // already handled in accordance with the TOSA spec.
    auto powFloat = std::pow(base.convertToFloat(), exp.convertToFloat());
    auto res = APFloat(powFloat);

    bool lostPrecision;
    res.convert(base.getSemantics(), APFloat::rmNearestTiesToEven,
                &lostPrecision);
    return res;
  }

  /// Called when the lhsValues.getElementType() is FloatType.
  DenseElementsAttr computeFloat(DenseElementsAttr lhsValues,
                                 DenseElementsAttr rhsValues,
                                 PatternRewriter &rewriter, PowOp op) const {
    return applyElementWise<APFloat, APFloat>(lhsValues, rhsValues,
                                              op.getType(), computePower);
  }

  bool isSupportedElementType(Type type) const {
    return type.isBF16() || type.isF16() || type.isF32();
  }
};

struct TosaFoldConstantMul
    : public TosaFoldConstantBinary<TosaFoldConstantMul, MulOp> {
  using TosaFoldConstantBinary<TosaFoldConstantMul,
                               MulOp>::TosaFoldConstantBinary;

  /// Called when the lhsValues.getElementType() is IntegerType.
  DenseElementsAttr computeInteger(DenseElementsAttr lhsValues,
                                   DenseElementsAttr rhsValues,
                                   PatternRewriter &rewriter, MulOp op) const {
    if (op.getShift() > 0) {
      (void)rewriter.notifyMatchFailure(
          op, "Non-zero shift folding is currently not implemented.");
      return {};
    }

    auto resultElementWidth =
        op.getType().getElementType().getIntOrFloatBitWidth();
    assert(resultElementWidth >=
               lhsValues.getElementType().getIntOrFloatBitWidth() &&
           "The multiplication is expected to have an at least as big output "
           "as input type");

    // Compute the multiplication and track if an overflow occurred to enable
    // emitting a warning
    bool mulOverflowed = false;
    auto newTensor = applyElementWise<APInt, APInt>(
        lhsValues, rhsValues, op.getType(),
        [&resultElementWidth, &mulOverflowed](const APInt &first,
                                              const APInt &second) {
          bool didOverflow;
          auto res = first.sext(resultElementWidth)
                         .smul_ov(second.sext(resultElementWidth), didOverflow);
          mulOverflowed |= didOverflow;
          return res;
        });
    if (mulOverflowed) {
      op.emitWarning(
          "Multiplication did overflow. The results are unspecified.");
    }
    return newTensor;
  }

  /// Called when the lhsValues.getElementType() is FloatType.
  DenseElementsAttr computeFloat(DenseElementsAttr lhsValues,
                                 DenseElementsAttr rhsValues,
                                 PatternRewriter &rewriter, MulOp op) const {
    return applyElementWise<APFloat, APFloat>(
        lhsValues, rhsValues, op.getType(),
        [](const APFloat &first, const APFloat &second) {
          return first * second;
        });
  }
};

struct TosaFoldConstantClamp
    : public TosaFoldConstantUnaryElementwise<TosaFoldConstantClamp, ClampOp> {
  using TosaFoldConstantUnaryElementwise<
      TosaFoldConstantClamp, ClampOp>::TosaFoldConstantUnaryElementwise;

  static void
  changeSemanticsLossless(APFloat &floatVal,
                          const llvm::fltSemantics *floatSemantics) {
    bool losesInfo;
    floatVal.convert(*floatSemantics, tosaRoundingMode, &losesInfo);
    assert(!losesInfo);
  }

  DenseElementsAttr applyClamp(DenseElementsAttr inputValues,
                               const APInt &lowerBound, const APInt &upperBound,
                               TensorType resultType) const {

    // Determine the width for the APInt comparison
    auto comparisonWidth =
        std::max(inputValues.getElementType().getIntOrFloatBitWidth(),
                 lowerBound.getBitWidth());
    // Sign-extend the upper and lower bound
    auto extUpperBound = upperBound.sext(comparisonWidth);
    auto extLowerBound = lowerBound.sext(comparisonWidth);

    // Determine the result type
    auto resultingIntType = cast<IntegerType>(resultType.getElementType());

    // Lambda to perform the clamp
    auto clampFun = [&extLowerBound, &extUpperBound,
                     &comparisonWidth](const APInt &val, IntegerType type) {
      auto clampedUpper =
          llvm::APIntOps::smin(val.sext(comparisonWidth), extUpperBound);
      auto fullyClamped = llvm::APIntOps::smax(clampedUpper, extLowerBound);
      assert(type.getWidth() >= fullyClamped.getSignificantBits());
      return fullyClamped.trunc(type.getWidth());
    };
    auto newTensor = applyElementWise<APInt, APInt, IntegerType>(
        inputValues, clampFun, resultingIntType);

    return newTensor;
  }

  DenseElementsAttr applyClamp(DenseElementsAttr inputValues,
                               APFloat lowerBound, APFloat upperBound,
                               TensorType resultType) const {
    auto inputValType = cast<FloatType>(inputValues.getElementType());
    auto inputWidth = inputValType.getWidth();
    auto bWidth = APFloat::semanticsSizeInBits(lowerBound.getSemantics());
    auto *comparisonSem = inputWidth < bWidth
                              ? &lowerBound.getSemantics()
                              : &inputValType.getFloatSemantics();

    changeSemanticsLossless(lowerBound, comparisonSem);
    changeSemanticsLossless(upperBound, comparisonSem);

    auto resultingFloatType = cast<FloatType>(resultType.getElementType());

    // Ensure that the value is larger than the lower bound and smaller than the
    // upper bound
    auto clampFun = [&lowerBound, &upperBound, &comparisonSem](APFloat val,
                                                               FloatType type) {
      if (val.isNaN()) {
        return APFloat::getNaN(type.getFloatSemantics());
      }
      changeSemanticsLossless(val, comparisonSem);
      auto clampedUpper = val < upperBound ? val : upperBound;
      auto fullyClamped = clampedUpper < lowerBound ? lowerBound : clampedUpper;
      changeSemanticsLossless(fullyClamped, &type.getFloatSemantics());
      return fullyClamped;
    };
    auto newTensor = applyElementWise<APFloat, APFloat, FloatType>(
        inputValues, clampFun, resultingFloatType);

    return newTensor;
  }

  /// Called when the values.getElementType() is IntegerType.
  DenseElementsAttr computeInteger(DenseElementsAttr values,
                                   PatternRewriter &rewriter,
                                   ClampOp op) const {
    if (isa<IntegerType>(values.getElementType()) &&
        cast<IntegerType>(values.getElementType()).isUnsigned()) {
      (void)rewriter.notifyMatchFailure(
          op, "Currently, unsigned integer clamps are unsupported.");
      return {};
    }

    auto lowerBoundVal = op.getMinIntAttr().getValue();
    auto upperBoundVal = op.getMaxIntAttr().getValue();
    assert(lowerBoundVal.getBitWidth() == upperBoundVal.getBitWidth());

    return applyClamp(values, lowerBoundVal, upperBoundVal, op.getType());
  }

  /// Called when the values.getElementType() is FloatType.
  DenseElementsAttr computeFloat(DenseElementsAttr values,
                                 PatternRewriter &rewriter, ClampOp op) const {
    auto lowerBoundVal = op.getMinFp();
    auto upperBoundVal = op.getMaxFp();
    assert(APFloat::getSizeInBits(lowerBoundVal.getSemantics()) ==
           APFloat::getSizeInBits(upperBoundVal.getSemantics()));

    return applyClamp(values, lowerBoundVal, upperBoundVal, op.getType());
  }
};

struct TosaFoldConstantCast : public TosaFoldConstantBase<CastOp> {

  using TosaFoldConstantBase::TosaFoldConstantBase;

  static APFloat convertIntToFloat(const APInt &toConvert,
                                   FloatType targetType) {
    APFloat res(targetType.getFloatSemantics());
    res.convertFromAPInt(toConvert, true /* isSigned */, tosaRoundingMode);
    return res;
  }

  static APFloat convertFloatToFloat(const APFloat &toConvert,
                                     FloatType targetType) {
    APFloat res(toConvert);
    bool didLosePrecision;
    res.convert(targetType.getFloatSemantics(), tosaRoundingMode,
                &didLosePrecision);
    return res;
  }

  static APInt convertFloatToInt(const APFloat &toConvert,
                                 IntegerType targetType) {
    auto targetWidth = targetType.getIntOrFloatBitWidth();
    // Converting NaN to an integer results in an unpredictable value. Pick 0.
    if (toConvert.isNaN()) {
      return APInt::getZero(targetWidth);
    }

    // Make sure to properly translate booleans
    if (targetWidth == 1) {
      return toConvert.isZero() ? APInt::getZero(1) : APInt::getAllOnes(1);
    }

    // Use the built-in functionality of APFloats to convert to integers.
    // The result of this conversion should be an integer which might still be
    // outside of the target integer range.
    auto floatSize = APFloat::getSizeInBits(toConvert.getSemantics());
    APSInt converted(std::max(floatSize, targetWidth), targetType.isUnsigned());
    bool ignored = false;
    toConvert.convertToInteger(converted, APFloat::rmNearestTiesToEven,
                               &ignored);
    // Clip to allowed range.
    if (targetWidth < floatSize) {
      if (targetType.isUnsigned()) {
        return converted.truncUSat(targetWidth);
      }
      return converted.truncSSat(targetWidth);
    }
    return converted;
  }

  static APInt convertIntToInt(const APInt &toConvert, IntegerType targetType) {
    // Make sure to properly translate booleans
    if (targetType.getWidth() == 1) {
      return toConvert.isZero() ? APInt::getZero(1) : APInt::getAllOnes(1);
    }
    if (targetType.isUnsigned()) {
      return toConvert.zextOrTrunc(targetType.getIntOrFloatBitWidth());
    }
    return toConvert.sextOrTrunc(targetType.getIntOrFloatBitWidth());
  }

  static void warnAboutNaNToIntCast(DenseElementsAttr elements, CastOp location,
                                    PatternRewriter &rewriter) {
    // This is only relevant if the input values are float
    if (!isa<FloatType>(elements.getElementType())) {
      return;
    }
    // Check if it is an float to integer conversion
    auto resultType = location.getOutput().getType();
    if (!isa<IntegerType>(cast<TensorType>(resultType).getElementType())) {
      return;
    }

    // Report encountered NaNs
    auto checkNan = [](const APFloat &val) { return val.isNaN(); };
    if (any_of(elements.getValues<APFloat>(), checkNan)) {
      location->emitWarning(
          "Float tensor is casted to integer and it contains NaN values. The "
          "cast results in an unspecified value.");
    }
  }

  LogicalResult matchAndRewrite(CastOp tosaCast,
                                PatternRewriter &rewriter) const override {
    auto inputTensor = tosaCast.getInput();

    // If the input tensor is not constant, we cannot fold it.
    if (failed(notifyIfNoTosaDenseConstantTensor(inputTensor, tosaCast,
                                                 rewriter))) {
      return failure();
    }

    auto fromType = inputTensor.getType().getElementType();
    auto toType = tosaCast.getOutput().getType().getElementType();

    DenseElementsAttr elements;
    matchPattern(inputTensor, m_Constant(&elements));

    // Issue a warning if we convert float -> int and NaNs are present; the
    // result value is unspecified in that case
    warnAboutNaNToIntCast(elements, tosaCast, rewriter);

    // Only fold splat tensors and those used only once to avoid duplicating
    // them.
    if (!inputTensor.hasOneUse() && !isa<SplatElementsAttr>(elements)) {
      return rewriter.notifyMatchFailure(tosaCast,
                                         "Currently, casts will only be folded "
                                         "if its input only has a single user");
    }

    // Report a match failure for unexpected types
    if (!toType.isIntOrFloat() || !fromType.isIntOrFloat()) {
      return rewriter.notifyMatchFailure(
          tosaCast, "Only casts from/to int/float are supported.");
    }

    auto isUnsigned = [](Type toCheck) {
      return isa<IntegerType>(toCheck) &&
             cast<IntegerType>(toCheck).isUnsigned();
    };
    auto typesToCheck = {toType, fromType};
    if (llvm::any_of(typesToCheck, isUnsigned)) {
      // TOSA casts currently don't support unsigned integers.
      // To support them by here, one could use APSInt instead of APInts,
      // however, this causes trouble with `getValues` which does not support
      // APSInts currently.
      return rewriter.notifyMatchFailure(
          tosaCast, "Cast folding from/to unsigned integers is not supported.");
    }

    DenseElementsAttr res;
    if (auto intOutTy = dyn_cast<IntegerType>(toType)) {
      if (isa<FloatType>(fromType)) {
        res = applyElementWise<APFloat, APInt, IntegerType>(
            elements, &convertFloatToInt, intOutTy);
      } else {
        assert(isa<IntegerType>(fromType));
        res = applyElementWise<APInt, APInt, IntegerType>(
            elements, &convertIntToInt, intOutTy);
      }
    } else {
      assert(isa<FloatType>(toType));
      auto floatOutTy = cast<FloatType>(toType);
      if (isa<FloatType>(fromType)) {
        res = applyElementWise<APFloat, APFloat, FloatType>(
            elements, &convertFloatToFloat, floatOutTy);
      } else {
        assert(isa<IntegerType>(fromType));
        res = applyElementWise<APInt, APFloat, FloatType>(
            elements, &convertIntToFloat, floatOutTy);
      }
    }

    rewriter.replaceOpWithNewOp<ConstOp>(tosaCast, res.getType(), res);
    return success();
  }
};

struct TosaFoldConstantFloatCasts : TosaFoldConstantCast {

  TosaFoldConstantFloatCasts(MLIRContext *ctx, bool foldSplatOrSingleUseOnly) : TosaFoldConstantCast(ctx, foldSplatOrSingleUseOnly) {}

  LogicalResult matchAndRewrite(CastOp tosaCast,
                                PatternRewriter &rewriter) const override {
    if (isa<IntegerType>(tosaCast.getInput().getType().getElementType())) {
      return rewriter.notifyMatchFailure(
          tosaCast, "Folding casts from int is currently disabled.");
    }

    return TosaFoldConstantCast::matchAndRewrite(tosaCast, rewriter);
  }
};

struct TosaFoldConstantAdd : public TosaFoldConstantBinary<TosaFoldConstantAdd, AddOp> {
  using TosaFoldConstantBinary<TosaFoldConstantAdd, AddOp>::TosaFoldConstantBinary;

  DenseElementsAttr computeInteger(DenseElementsAttr lhsValues,
                                   DenseElementsAttr rhsValues,
                                   PatternRewriter &rewriter, AddOp op) const {
    bool addOverflowed = false;
    auto intAdd = [&addOverflowed](const APInt &first, const APInt &second) {
      bool didOverflow;
      auto res = first.sadd_ov(second, didOverflow);
      addOverflowed |= didOverflow;
      return res;
    };
    auto newTensor = applyElementWise<APInt, APInt>(lhsValues, rhsValues,
                                                    op.getType(), intAdd);
    if (addOverflowed) {
      op->emitWarning("Addition did overflow. The results are unspecified.");
    }
    return newTensor;
  }

  DenseElementsAttr computeFloat(DenseElementsAttr lhsValues,
                                 DenseElementsAttr rhsValues,
                                 PatternRewriter &rewriter, AddOp op) const {
    auto floatAdd = [](const APFloat &first, const APFloat &second) {
      return first + second;
    };
    return applyElementWise<APFloat, APFloat>(lhsValues, rhsValues,
                                              op.getType(), floatAdd);
  }
};

struct TosaFoldConstantSub : public TosaFoldConstantBinary<TosaFoldConstantSub, SubOp> {
  using TosaFoldConstantBinary<TosaFoldConstantSub, SubOp>::TosaFoldConstantBinary;

  DenseElementsAttr computeInteger(DenseElementsAttr lhsValues,
                                   DenseElementsAttr rhsValues,
                                   PatternRewriter &rewriter, SubOp op) const {
    bool overflowed = false;
    auto newTensor = applyElementWise<APInt, APInt>(lhsValues, rhsValues,
                                                    op.getType(), [&overflowed](const APInt &first, const APInt &second) {
      bool didOverflow;
      auto res = first.ssub_ov(second, didOverflow);
      overflowed |= didOverflow;
      return res;
    });

    if (overflowed) {
      op->emitWarning("Subtraction did overflow. The results are unspecified.");
    }
    return newTensor;
  }

  DenseElementsAttr computeFloat(DenseElementsAttr lhsValues,
                                 DenseElementsAttr rhsValues,
                                 PatternRewriter &rewriter, SubOp op) const {
    return applyElementWise<APFloat, APFloat>(lhsValues, rhsValues,
                                              op.getType(), [](const APFloat &first, const APFloat &second) {
      return first - second;
    });
  }
};

struct TosaFoldConstantGreater : public TosaFoldConstantBinary<TosaFoldConstantGreater, GreaterOp> {
  using TosaFoldConstantBinary<TosaFoldConstantGreater, GreaterOp>::TosaFoldConstantBinary;

  DenseElementsAttr computeInteger(DenseElementsAttr lhsValues,
                                   DenseElementsAttr rhsValues,
                                   PatternRewriter &rewriter,
                                   GreaterOp op) const {
    return applyElementWise<APInt, APInt>(
        lhsValues, rhsValues, op.getType(),
        [](const APInt &first, const APInt &second) {
          return APInt(1, first.sgt(second));
        });
  }

  DenseElementsAttr computeFloat(DenseElementsAttr lhsValues,
                                 DenseElementsAttr rhsValues,
                                 PatternRewriter &rewriter,
                                 GreaterOp op) const {
      return applyElementWise<APFloat, APInt>(
          lhsValues, rhsValues, op.getType(),
          [](const APFloat &first, const APFloat &second) {
            if (first.isNaN() || second.isNaN())
              return APInt(1, false);
            return APInt(1, first > second);
          });
  }
};

struct TosaFoldConstantBitwiseNot
    : public TosaFoldConstantUnaryElementwise<TosaFoldConstantBitwiseNot,
                                              BitwiseNotOp> {
  using TosaFoldConstantUnaryElementwise<
      TosaFoldConstantBitwiseNot,
      BitwiseNotOp>::TosaFoldConstantUnaryElementwise;

  DenseElementsAttr computeInteger(DenseElementsAttr values,
                                   PatternRewriter &rewriter, TosaOp op) const {
    return applyElementWise<APInt, APInt, IntegerType>(
        values, [](const APInt &val, IntegerType) { return ~val; },
        cast<IntegerType>(values.getElementType()));
  }
};

struct TosaFoldConstantCeil
    : public TosaFoldConstantUnaryElementwise<TosaFoldConstantCeil, CeilOp> {
  using TosaFoldConstantUnaryElementwise<
      TosaFoldConstantCeil, CeilOp>::TosaFoldConstantUnaryElementwise;

  DenseElementsAttr computeFloat(DenseElementsAttr values,
                                 PatternRewriter &rewriter, TosaOp op) const {
    return applyElementWise<APFloat, APFloat, FloatType>(
        values,
        [](const APFloat &val, FloatType) {
          auto res = val;
          res.roundToIntegral(llvm::RoundingMode::TowardPositive);
          return res;
        },
        cast<FloatType>(values.getElementType()));
  }
};

struct TosaFoldConstantErf
    : public TosaFoldConstantUnaryElementwise<TosaFoldConstantErf, ErfOp> {
  using TosaFoldConstantUnaryElementwise<
      TosaFoldConstantErf, ErfOp>::TosaFoldConstantUnaryElementwise;

  DenseElementsAttr computeFloat(DenseElementsAttr values,
                                 PatternRewriter &rewriter, TosaOp op) const {
    return applyElementWise<APFloat, APFloat, FloatType>(
        values,
        [](const APFloat &val, FloatType) {
          auto res = APFloat(std::erf(val.convertToFloat()));
          bool lostPrecision;
          res.convert(val.getSemantics(), APFloat::rmNearestTiesToEven,
                      &lostPrecision);
          return res;
        },
        cast<FloatType>(values.getElementType()));
  }

  bool isSupportedElementType(Type type) const {
    // Note: For now, we only support BF16 and F32 as std::erf may
    // have an impact on the accuracy of the returned value.
    return type.isBF16() || type.isF32();
  }
};

struct TosaFoldConstantLog
    : public TosaFoldConstantUnaryElementwise<TosaFoldConstantLog, LogOp> {
  using TosaFoldConstantUnaryElementwise<
      TosaFoldConstantLog, LogOp>::TosaFoldConstantUnaryElementwise;

  DenseElementsAttr computeFloat(DenseElementsAttr values,
                                 PatternRewriter &rewriter, TosaOp op) const {
    return applyElementWise<APFloat, APFloat, FloatType>(
        values,
        [](const APFloat &val, FloatType) {
          auto res = APFloat(std::log(val.convertToFloat()));
          bool lostPrecision;
          res.convert(val.getSemantics(), APFloat::rmNearestTiesToEven,
                      &lostPrecision);
          return res;
        },
        cast<FloatType>(values.getElementType()));
  }

  bool isSupportedElementType(Type type) const {
    // convertToFloat uses F32, so we specify the supported types to make sure
    // to properly handle F64 if needed in the future.
    return type.isBF16() || type.isF16() || type.isF32();
  }
};

struct TosaFoldConstantBitwiseAnd
    : public TosaFoldConstantBinary<TosaFoldConstantBitwiseAnd, BitwiseAndOp> {
  using TosaFoldConstantBinary<TosaFoldConstantBitwiseAnd,
                               BitwiseAndOp>::TosaFoldConstantBinary;

  DenseElementsAttr computeInteger(DenseElementsAttr lhsValues,
                                   DenseElementsAttr rhsValues,
                                   PatternRewriter &rewriter,
                                   BitwiseAndOp op) const {
    return applyElementWise<APInt, APInt>(
        lhsValues, rhsValues, op.getType(),
        [](const APInt &lhs, const APInt &rhs) { return lhs & rhs; });
  }
};

struct TosaFoldConstantBitwiseOr
    : public TosaFoldConstantBinary<TosaFoldConstantBitwiseOr, BitwiseOrOp> {
  using TosaFoldConstantBinary<TosaFoldConstantBitwiseOr,
                               BitwiseOrOp>::TosaFoldConstantBinary;

  DenseElementsAttr computeInteger(DenseElementsAttr lhsValues,
                                   DenseElementsAttr rhsValues,
                                   PatternRewriter &rewriter,
                                   BitwiseOrOp op) const {
    return applyElementWise<APInt, APInt>(
        lhsValues, rhsValues, op.getType(),
        [](const APInt &lhs, const APInt &rhs) { return lhs | rhs; });
  }
};

struct TosaFoldConstantGreaterEqual
    : public TosaFoldConstantBinary<TosaFoldConstantGreaterEqual,
                                    GreaterEqualOp> {
  using TosaFoldConstantBinary<TosaFoldConstantGreaterEqual,
                               GreaterEqualOp>::TosaFoldConstantBinary;

  DenseElementsAttr computeInteger(DenseElementsAttr lhsValues,
                                   DenseElementsAttr rhsValues,
                                   PatternRewriter &rewriter,
                                   GreaterEqualOp op) const {
    return applyElementWise<APInt, APInt>(
        lhsValues, rhsValues, op.getType(),
        [](const APInt &first, const APInt &second) {
          return APInt(1, first.sge(second));
        });
  }

  DenseElementsAttr computeFloat(DenseElementsAttr lhsValues,
                                 DenseElementsAttr rhsValues,
                                 PatternRewriter &rewriter,
                                 GreaterEqualOp op) const {
    return applyElementWise<APFloat, APInt>(
        lhsValues, rhsValues, op.getType(),
        [](const APFloat &first, const APFloat &second) {
          if (first.isNaN() || second.isNaN())
            return APInt(1, false);
          return APInt(1, first >= second);
        });
  }
};

struct TosaFoldConstantEqual
    : public TosaFoldConstantBinary<TosaFoldConstantEqual, EqualOp> {
  using TosaFoldConstantBinary<TosaFoldConstantEqual,
                               EqualOp>::TosaFoldConstantBinary;

  DenseElementsAttr computeInteger(DenseElementsAttr lhsValues,
                                   DenseElementsAttr rhsValues,
                                   PatternRewriter &rewriter,
                                   EqualOp op) const {
    return applyElementWise<APInt, APInt>(
        lhsValues, rhsValues, op.getType(),
        [](const APInt &first, const APInt &second) {
          return APInt(1, first.eq(second));
        });
  }

  DenseElementsAttr computeFloat(DenseElementsAttr lhsValues,
                                 DenseElementsAttr rhsValues,
                                 PatternRewriter &rewriter, EqualOp op) const {
    return applyElementWise<APFloat, APInt>(
        lhsValues, rhsValues, op.getType(),
        [](const APFloat &first, const APFloat &second) {
          return APInt(1, first == second);
        });
  }
};

struct TosaFoldConstantMinimum
    : public TosaFoldConstantBinary<TosaFoldConstantMinimum, MinimumOp> {
  using TosaFoldConstantBinary<TosaFoldConstantMinimum,
                               MinimumOp>::TosaFoldConstantBinary;

  DenseElementsAttr computeInteger(DenseElementsAttr lhsValues,
                                   DenseElementsAttr rhsValues,
                                   PatternRewriter &rewriter,
                                   MinimumOp op) const {
    return applyElementWise<APInt, APInt>(
        lhsValues, rhsValues, op.getType(),
        [](const APInt &first, const APInt &second) {
          return first.slt(second) ? first : second;
        });
  }

  DenseElementsAttr computeFloat(DenseElementsAttr lhsValues,
                                 DenseElementsAttr rhsValues,
                                 PatternRewriter &rewriter,
                                 MinimumOp op) const {
    return applyElementWise<APFloat, APFloat>(
        lhsValues, rhsValues, op.getType(),
        [](const APFloat &first, const APFloat &second) {
          if (first.isNaN() || second.isNaN())
            return first.isNaN() ? first : second;
          return first < second ? first : second;
        });
  }
};

struct TosaFoldConstantMaximum
    : public TosaFoldConstantBinary<TosaFoldConstantMaximum, MaximumOp> {
  using TosaFoldConstantBinary<TosaFoldConstantMaximum,
                               MaximumOp>::TosaFoldConstantBinary;

  DenseElementsAttr computeInteger(DenseElementsAttr lhsValues,
                                   DenseElementsAttr rhsValues,
                                   PatternRewriter &rewriter,
                                   MaximumOp op) const {
    return applyElementWise<APInt, APInt>(
        lhsValues, rhsValues, op.getType(),
        [](const APInt &first, const APInt &second) {
          return first.sgt(second) ? first : second;
        });
  }

  DenseElementsAttr computeFloat(DenseElementsAttr lhsValues,
                                 DenseElementsAttr rhsValues,
                                 PatternRewriter &rewriter,
                                 MaximumOp op) const {
    return applyElementWise<APFloat, APFloat>(
        lhsValues, rhsValues, op.getType(),
        [](const APFloat &first, const APFloat &second) {
          if (first.isNaN() || second.isNaN())
            return first.isNaN() ? first : second;
          return first > second ? first : second;
        });
  }
};

template <typename BaseType>
DenseElementsAttr padType(ShapedType inputType, ElementsAttr inputValues,
                          DenseElementsAttr paddings,
                          std::optional<DenseElementsAttr> padConstValue,
                          ShapedType outputType, BaseType zero) {
  BaseType padConst(zero);
  if (padConstValue.has_value())
    padConst = padConstValue.value().getSplatValue<BaseType>();

  auto values = inputValues.getValues<BaseType>();
  auto paddingVals = paddings.getValues<int64_t>();

  auto outputShape = outputType.getShape();
  auto inputShape = inputType.getShape();

  // Implements the logic from
  // https://www.mlplatform.org/tosa/tosa_spec.html#_pad
  SmallVector<BaseType> outputValues(outputType.getNumElements(), padConst);
  for (size_t outIndex = 0, e = outputValues.size(); outIndex < e; ++outIndex) {
    auto indexInTarget = offsetToIndex(outputShape, outIndex);

    bool isPad =
        llvm::any_of(llvm::enumerate(indexInTarget), [&](const auto &dimInfo) {
          auto index = dimInfo.index();
          auto i = dimInfo.value() - paddingVals[index * 2];
          return static_cast<bool>(i < 0 || i >= inputShape[index]);
        });

    auto inputIndexOffset = indexToOffset(outputShape, indexInTarget);
    outputValues[outIndex] = isPad ? padConst : values[inputIndexOffset];
  }
  return DenseElementsAttr::get(outputType,
                                llvm::ArrayRef<BaseType>(outputValues));
}

DenseElementsAttr pad(ShapedType inputType, ElementsAttr inputValues,
                      DenseElementsAttr paddings,
                      std::optional<DenseElementsAttr> padConstValue,
                      ShapedType outputType) {

  auto baseType = inputType.getElementType();

  // Handle integer types with APInt
  if (auto intType = dyn_cast<IntegerType>(baseType))
    return padType<APInt>(inputType, inputValues, paddings, padConstValue,
                          outputType,
                          APInt(baseType.getIntOrFloatBitWidth(), 0));

  assert(isa<FloatType>(baseType) && "Unknown element type.");
  FloatType fpType = cast<FloatType>(baseType);

  // Handle FP types with APFloat
  APFloat zero(fpType.getFloatSemantics(), APInt::getZero(fpType.getWidth()));
  return padType<APFloat>(inputType, inputValues, paddings, padConstValue,
                          outputType, zero);
}

struct TosaFoldConstantPad : public TosaFoldConstantBase<tosa::PadOp> {
  using TosaFoldConstantBase::TosaFoldConstantBase;

  LogicalResult matchAndRewrite(tosa::PadOp op,
                                PatternRewriter &rewriter) const override {
    auto outputType = cast<ShapedType>(op.getType());
    // TOSA doesn't support quantized types.
    if (!outputType.getElementType().isIntOrIndexOrFloat())
      return failure();

    auto input = op.getInput1();
    ElementsAttr inputValues;
    if (!matchPattern(input, m_Constant(&inputValues)))
      return failure();

    // Only fold op with multiple users if foldSplatOrSingleUseOnly == true.
    if (!llvm::hasSingleElement(input.getDefiningOp()->getUsers()) &&
        foldSplatOrSingleUseOnly)
      return failure();

    std::optional<DenseElementsAttr> padConstValue;
    if (op.getPadConst()) {
      DenseElementsAttr attr;
      if (!matchPattern(op.getPadConst(), m_Constant(&attr)))
        return failure();
      padConstValue = attr;
    }

    DenseElementsAttr paddings;
    if (!matchPattern(op.getPadding(), m_Constant(&paddings)))
      return failure();

    auto resultAttr =
        pad(input.getType(), inputValues, paddings, padConstValue, outputType);
    rewriter.replaceOpWithNewOp<tosa::ConstOp>(op, outputType, resultAttr);

    return success();
  }
};

} // namespace

void mlir::tosa::populateTosaFoldConstantPatterns(
    MLIRContext *ctx, RewritePatternSet &patterns,
    bool foldSplatOrSingleUseOnly,
    bool enableIntCastFolding) {
  patterns.add<TosaFoldConstantTranspose>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantReciprocal>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantRSQRT>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantLogicalNot>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantPow>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantMul>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantClamp>(ctx, foldSplatOrSingleUseOnly);
  if (enableIntCastFolding) {
    patterns.add<TosaFoldConstantCast>(ctx, foldSplatOrSingleUseOnly);
  } else {
    patterns.add<TosaFoldConstantFloatCasts>(ctx, foldSplatOrSingleUseOnly);
  }
  patterns.add<TosaFoldConstantAdd>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantSub>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantGreater>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantBitwiseNot>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantCeil>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantErf>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantLog>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantBitwiseAnd>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantBitwiseOr>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantGreaterEqual>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantEqual>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantMinimum>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantMaximum>(ctx, foldSplatOrSingleUseOnly);
  patterns.add<TosaFoldConstantPad>(ctx, foldSplatOrSingleUseOnly);
}

