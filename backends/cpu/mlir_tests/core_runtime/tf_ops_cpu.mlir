// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// RUN: tfrt_translate -mlir-to-bef %s | bef_executor -devices=cpu | FileCheck %s --dump-input=fail

// CHECK: --- Running 'const_f32'
func @const_f32() -> !hex.chain {
 %ch0 = hex.new.chain
  %cpu = corert.get_device "cpu"

  %cpu_handle_result = corert.executeop(%cpu)
    "tf.Const"() {value = dense<[-1.0, -0.5, 0.0, 0.5, 1.0]> : tensor<5xf32>, dtype = f32} : 1

  // CHECK: DenseHostTensor dtype = F32 shape = [5], values = [-1.000000e+00, -5.000000e-01, 0.000000e+00, 5.000000e-01, 1.000000e+00]
  %ch_print_cpu = corert.executeop.seq(%cpu, %ch0) "tfrt_test.print"(%cpu_handle_result) : 0
  hex.return %ch_print_cpu : !hex.chain
}
