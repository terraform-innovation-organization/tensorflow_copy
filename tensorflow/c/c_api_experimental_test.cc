/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/c/c_api_experimental.h"

#include "absl/types/optional.h"
#include "tensorflow/c/c_api.h"
#include "tensorflow/c/c_api_internal.h"
#include "tensorflow/c/c_test_util.h"
#include "tensorflow/c/eager/c_api.h"
#include "tensorflow/c/eager/c_api_test_util.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/resource_loader.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/protobuf/tensorflow_server.pb.h"

namespace tensorflow {
namespace {

TEST(CAPI_EXPERIMENTAL, GetServerDefTest) {
  const string expected_text_proto(R"(cluster {
  job {
    name: "worker"
    tasks {
      key: 0
      value: "tpuserver:0"
    }
    tasks {
      key: 1
      value: "localhost:1"
    }
  }
}
job_name: "worker"
task_index: 1
protocol: "grpc"
)");

  TF_Status* status = TF_NewStatus();
  TF_Buffer* result = TFE_GetServerDef(expected_text_proto.c_str(), status);
  EXPECT_EQ(TF_GetCode(status), TF_OK);

  ServerDef actual;
  ASSERT_TRUE(actual.ParseFromArray(result->data, result->length));
  string actual_text_proto;
  tensorflow::protobuf::TextFormat::PrintToString(actual, &actual_text_proto);
  EXPECT_EQ(expected_text_proto, actual_text_proto);

  const string malformed_text_proto(R"(cluster {
  job {
    name: "worker")");
  TF_Buffer* null_result =
      TFE_GetServerDef(malformed_text_proto.c_str(), status);
  EXPECT_NE(TF_GetCode(status), TF_OK);
  EXPECT_TRUE(absl::StrContains(TF_Message(status),
                                "Invalid text proto for ServerDef"));
  EXPECT_EQ(null_result, nullptr);

  // Cleanup
  TF_DeleteBuffer(result);
  TF_DeleteStatus(status);
}

TEST(CAPI_EXPERIMENTAL, IsStateful) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  int assign = TF_OpIsStateful("AssignAddVariableOp", status.get());
  ASSERT_EQ(TF_OK, TF_GetCode(status.get())) << TF_Message(status.get());
  EXPECT_EQ(assign, 1);
  int id = TF_OpIsStateful("Identity", status.get());
  ASSERT_EQ(TF_OK, TF_GetCode(status.get())) << TF_Message(status.get());
  EXPECT_EQ(id, 0);
}

class ShapeInferenceTest : public ::testing::Test {
 protected:
  ShapeInferenceTest()
      : status_(TF_NewStatus()), tfe_context_options_(TFE_NewContextOptions()) {
    tfe_context_ = TFE_NewContext(tfe_context_options_, status_);
    CHECK_EQ(TF_OK, TF_GetCode(status_)) << TF_Message(status_);
  }

  ~ShapeInferenceTest() override {
    TFE_DeleteContextOptions(tfe_context_options_);
    TFE_DeleteContext(tfe_context_);
    TF_DeleteStatus(status_);
  }

  // Checks the expected result of shape inference for the given `op`.
  void CheckOutputShapes(
      TFE_Op* op,
      const std::vector<absl::optional<std::vector<int64_t>>>& input_shapes_vec,
      const std::vector<TF_Tensor*>& input_tensors,
      const absl::optional<std::vector<int64_t>>& expected_shape) {
    // Create input_shapes.
    TF_ShapeAndTypeList* input_shapes =
        TF_NewShapeAndTypeList(input_shapes_vec.size());
    for (size_t i = 0; i < input_shapes_vec.size(); ++i) {
      const auto& input_shape = input_shapes_vec[i];
      if (input_shape.has_value()) {
        TF_ShapeAndTypeListSetShape(input_shapes, i, input_shape->data(),
                                    input_shape->size());
      } else {
        TF_ShapeAndTypeListSetUnknownShape(input_shapes, i);
      }
    }
    TF_ShapeAndTypeList* output_shapes;
    TFE_InferShapes(op, input_shapes,
                    input_tensors.empty()
                        ? nullptr
                        : const_cast<TF_Tensor**>(input_tensors.data()),
                    /*input_tensors_as_shapes*/ nullptr,
                    /*input_resource_shapes_and_types*/ nullptr, &output_shapes,
                    /*output_resource_shapes_and_types*/ nullptr, status_);
    CHECK_EQ(TF_OK, TF_GetCode(status_)) << TF_Message(status_);
    CHECK_EQ(output_shapes->num_items, 1);

    int num_dims = output_shapes->items[0].num_dims;
    int64_t* dims = output_shapes->items[0].dims;

    if (!expected_shape.has_value()) {
      EXPECT_EQ(num_dims, -1);
      EXPECT_EQ(dims, nullptr);
      return;
    }

    EXPECT_EQ(num_dims, expected_shape->size());
    for (size_t i = 0; i < num_dims; ++i) {
      EXPECT_EQ(dims[i], (*expected_shape)[i]);
    }
    TF_DeleteShapeAndTypeList(input_shapes);
    TF_DeleteShapeAndTypeList(output_shapes);
  }

  absl::optional<std::vector<int64_t>> make_shape(
      std::vector<int64_t>&& dims) const {
    return absl::make_optional(dims);
  }

  absl::optional<std::vector<int64_t>> unknown_shape() const {
    return absl::nullopt;
  }

  static constexpr int64_t kUnknownDim =
      shape_inference::InferenceContext::kUnknownDim;
  TF_Status* status_;
  TFE_ContextOptions* tfe_context_options_;
  TFE_Context* tfe_context_;
};

TEST_F(ShapeInferenceTest, InfersShapesFromInputShapes) {
  TFE_Op* matmul_op;
  matmul_op = TFE_NewOp(tfe_context_, "MatMul", status_);
  CHECK_EQ(TF_OK, TF_GetCode(status_)) << TF_Message(status_);

  // Infer shape when everything is known.
  CheckOutputShapes(matmul_op,
                    /*input_shapes*/ {make_shape({3, 2}), make_shape({2, 4})},
                    /*input_tensors*/ {},
                    /*expected_shape*/ make_shape({3, 4}));

  // Infer shape when second operand has unknown shape.
  CheckOutputShapes(matmul_op,
                    /*input_shapes*/ {make_shape({3, 2}), unknown_shape()},
                    /*input_tensors*/ {},
                    /*expected_shape*/ make_shape({3, kUnknownDim}));

  // Infer shape when some dimensions are unknown.
  CheckOutputShapes(
      matmul_op,
      /*input_shapes*/ {make_shape({kUnknownDim, 2}), make_shape({2, 4})},
      /*input_tensors*/ {},
      /*expected_shape*/ make_shape({kUnknownDim, 4}));

  // Infer shape when everything is unknown.
  CheckOutputShapes(matmul_op,
                    /*input_shapes*/ {unknown_shape(), unknown_shape()},
                    /*input_tensors*/ {},
                    /*expected_shape*/ make_shape({kUnknownDim, kUnknownDim}));

  TFE_DeleteOp(matmul_op);
  // TODO(bgogul): Add some death tests where status is not OK.
}

TEST_F(ShapeInferenceTest, InfersShapesFromInputTensors) {
  // Prepare some tensors for shape.
  TF_Tensor* tensor_1X6 = Int32Tensor({1, 6});
  CHECK_EQ(TF_OK, TF_GetCode(status_)) << TF_Message(status_);
  TF_Tensor* tensor_1X1X6 = Int32Tensor({1, 1, 6});
  CHECK_EQ(TF_OK, TF_GetCode(status_)) << TF_Message(status_);

  TFE_Op* reshape_op = TFE_NewOp(tfe_context_, "Reshape", status_);
  CHECK_EQ(TF_OK, TF_GetCode(status_)) << TF_Message(status_);
  TFE_OpSetAttrType(reshape_op, "T", TF_FLOAT);
  TFE_OpSetAttrType(reshape_op, "Tshape", TF_INT32);
  CheckOutputShapes(reshape_op,
                    /* input_shapes*/ {unknown_shape(), unknown_shape()},
                    /* input_tensors*/ {nullptr, tensor_1X6},
                    /*expected_shape*/ make_shape({1, 6}));
  TFE_DeleteOp(reshape_op);
  reshape_op = nullptr;

  TFE_Op* fill_op = TFE_NewOp(tfe_context_, "Fill", status_);
  CHECK_EQ(TF_OK, TF_GetCode(status_)) << TF_Message(status_);
  TFE_OpSetAttrType(fill_op, "T", TF_FLOAT);
  TFE_OpSetAttrType(fill_op, "Tshape", TF_INT32);

  float five = 5.0;
  TFE_TensorHandle* scalar = TestScalarTensorHandle(tfe_context_, five);
  TF_Tensor* scalarTensor = TFE_TensorHandleResolve(scalar, status_);
  CHECK_EQ(TF_OK, TF_GetCode(status_)) << TF_Message(status_);
  CheckOutputShapes(fill_op,
                    /* input_shapes*/ {unknown_shape(), unknown_shape()},
                    /* input_tensors*/ {tensor_1X1X6, scalarTensor},
                    /*expected_shape*/ make_shape({1, 1, 6}));
  TFE_DeleteOp(fill_op);
  fill_op = nullptr;

  TFE_DeleteTensorHandle(scalar);
  TF_DeleteTensor(scalarTensor);
  TF_DeleteTensor(tensor_1X1X6);
  TF_DeleteTensor(tensor_1X6);
}

TEST(CAPI_EXPERIMENTAL, LibraryPluggableDeviceLoadFunctions) {
  // TODO(penpornk): Enable this test on Windows.
#if !defined(PLATFORM_WINDOWS)
#if !defined(TENSORFLOW_NO_SHARED_OBJECTS)
  // Load the library.
  TF_Status* status = TF_NewStatus();
  string lib_path =
      tensorflow::GetDataDependencyFilepath(tensorflow::io::JoinPath(
          "tensorflow", "c", "experimental", "stream_executor", "test",
          "test_pluggable_device.so"));
  TF_Library* lib = TF_LoadPluggableDeviceLibrary(lib_path.c_str(), status);
  TF_Code code = TF_GetCode(status);
  string status_msg(TF_Message(status));
  TF_DeleteStatus(status);
  ASSERT_EQ(TF_OK, code) << status_msg;
  TF_DeletePluggableDeviceLibraryHandle(lib);
#endif  // !defined(TENSORFLOW_NO_SHARED_OBJECTS)
#endif  // !defined(PLATFORM_WINDOWS)
}

TEST(CAPI_EXPERIMENTAL, LibraryNextPluggableDeviceLoadFunctions) {
  // TODO(penpornk): Enable this test on Windows.
#if !defined(PLATFORM_WINDOWS)
#if !defined(TENSORFLOW_NO_SHARED_OBJECTS)
  // Load the library.
  TF_Status* status = TF_NewStatus();
  string lib_path =
      tensorflow::GetDataDependencyFilepath(tensorflow::io::JoinPath(
          "tensorflow", "core", "common_runtime", "next_pluggable_device", "c",
          "test_next_pluggable_device_plugin.so"));
  TF_Library* lib = TF_LoadPluggableDeviceLibrary(lib_path.c_str(), status);
  TF_Code code = TF_GetCode(status);
  string status_msg(TF_Message(status));
  TF_DeleteStatus(status);
  ASSERT_EQ(TF_OK, code) << status_msg;
  TF_DeletePluggableDeviceLibraryHandle(lib);
#endif  // !defined(TENSORFLOW_NO_SHARED_OBJECTS)
#endif  // !defined(PLATFORM_WINDOWS)
}

void DefineFunction(const char* name, TF_Function** func,
                    const char* description = nullptr,
                    bool append_hash = false) {
  std::unique_ptr<TF_Graph, decltype(&TF_DeleteGraph)> func_graph(
      TF_NewGraph(), TF_DeleteGraph);
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> s(TF_NewStatus(),
                                                           TF_DeleteStatus);

  TF_Operation* feed = Placeholder(func_graph.get(), s.get());
  TF_Operation* neg = Neg(feed, func_graph.get(), s.get());

  TF_Output inputs[] = {{feed, 0}};
  TF_Output outputs[] = {{neg, 0}};
  *func = TF_GraphToFunction(func_graph.get(), name, append_hash, -1,
                             /*opers=*/nullptr, 1, inputs, 1, outputs,
                             /*output_names=*/nullptr,
                             /*opts=*/nullptr, description, s.get());
  ASSERT_EQ(TF_OK, TF_GetCode(s.get())) << TF_Message(s.get());
  ASSERT_NE(*func, nullptr);
}

class CApiExperimentalFunctionTest : public ::testing::Test {
 protected:
  CApiExperimentalFunctionTest()
      : s_(TF_NewStatus()), func_graph_(TF_NewGraph()), func_(nullptr) {}

  void SetUp() override {}

  ~CApiExperimentalFunctionTest() override {
    TF_DeleteFunction(func_);
    TF_DeleteGraph(func_graph_);
    TF_DeleteStatus(s_);
  }

  const char* func_name_ = "MyFunc";
  TF_Status* s_;
  TF_Graph* func_graph_;
  TF_Function* func_;
};

TEST_F(CApiExperimentalFunctionTest, GraphRemoveFunction) {
  TF_Function* funcs[1];
  DefineFunction(func_name_, &func_);

  TF_GraphCopyFunction(func_graph_, func_, nullptr, s_);
  ASSERT_EQ(TF_OK, TF_GetCode(s_)) << TF_Message(s_);

  EXPECT_EQ(TF_GraphNumFunctions(func_graph_), 1);
  EXPECT_EQ(TF_GraphGetFunctions(func_graph_, funcs, 1, s_), 1);
  ASSERT_EQ(TF_OK, TF_GetCode(s_)) << TF_Message(s_);

  TF_GraphRemoveFunction(func_graph_, func_name_, s_);
  ASSERT_EQ(TF_OK, TF_GetCode(s_)) << TF_Message(s_);

  EXPECT_EQ(TF_GraphNumFunctions(func_graph_), 0);
  EXPECT_EQ(TF_GraphGetFunctions(func_graph_, funcs, 1, s_), 0);

  TF_DeleteFunction(funcs[0]);
}

TEST_F(CApiExperimentalFunctionTest, EmptyGraphRemoveNonExistentFunction) {
  TF_GraphRemoveFunction(func_graph_, "wrong_name", s_);
  EXPECT_EQ(TF_INVALID_ARGUMENT, TF_GetCode(s_));
  EXPECT_EQ(string("Tried to remove non-existent function 'wrong_name'."),
            string(TF_Message(s_)));
}

TEST_F(CApiExperimentalFunctionTest, GraphRemoveNonExistentFunction) {
  TF_Function* funcs[1];
  DefineFunction(func_name_, &func_);

  TF_GraphCopyFunction(func_graph_, func_, nullptr, s_);
  ASSERT_EQ(TF_OK, TF_GetCode(s_)) << TF_Message(s_);

  EXPECT_EQ(TF_GraphNumFunctions(func_graph_), 1);
  EXPECT_EQ(TF_GraphGetFunctions(func_graph_, funcs, 1, s_), 1);
  ASSERT_EQ(TF_OK, TF_GetCode(s_)) << TF_Message(s_);

  TF_GraphRemoveFunction(func_graph_, "wrong_name", s_);
  EXPECT_EQ(TF_INVALID_ARGUMENT, TF_GetCode(s_));
  EXPECT_EQ(string("Tried to remove non-existent function 'wrong_name'."),
            string(TF_Message(s_)));
  TF_DeleteFunction(funcs[0]);
}

}  // namespace
}  // namespace tensorflow
