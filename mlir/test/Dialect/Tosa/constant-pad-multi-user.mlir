// RUN: mlir-opt --split-input-file --tosa-layerwise-constant-fold="fold-splat-or-single-use-only=0" %s | FileCheck %s

// CHECK-LABEL: @pad_int32_multi_user
func.func @pad_int32_multi_user() -> (tensor<2x2xi32>, tensor<5x5xi32>) {
  // CHECK: "tosa.const"() <{value = dense<{{\[\[}}1, 1, 1, 1, 1], [1, 2, 2, 1, 1], [1, 2, 2, 1, 1], [1, 1, 1, 1, 1], [1, 1, 1, 1, 1]]>
  // CHECK: "tosa.const"() <{value = dense<2> : tensor<2x2xi32>}>
  // CHECK-NOT: "tosa.pad"
  %0 = "tosa.const"() {value = dense<2> : tensor<2x2xi32>} : () -> tensor<2x2xi32>
  %5 = "tosa.const"() {value = dense<[[1, 2], [1, 2]]> : tensor<2x2xi64>} : () -> tensor<2x2xi64>
  %6 = "tosa.const"() {value = dense<1> : tensor<i32>} : () -> tensor<i32>
  %1 = "tosa.pad"(%0, %5, %6) : (tensor<2x2xi32>, tensor<2x2xi64>, tensor<i32>) -> tensor<5x5xi32>
  return %0, %1 : tensor<2x2xi32>, tensor<5x5xi32>
}