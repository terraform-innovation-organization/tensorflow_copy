/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/cc/experimental/libtf/runtime/core/core.h"
#include "tensorflow/cc/experimental/libtf/tests/runtime_test.h"

namespace tf {
namespace libtf {
namespace runtime {

INSTANTIATE_TEST_SUITE_P(TF2CAPI, RuntimeTest,
                         ::testing::Values(core::Runtime));
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RuntimeTest);
}  // namespace runtime
}  // namespace libtf
}  // namespace tf
