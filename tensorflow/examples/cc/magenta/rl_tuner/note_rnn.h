/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");

You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_EXAMPLES_CC_MAGENTA_RL_TUNER_NOTE_RNN_H_
#define TENSORFLOW_EXAMPLES_CC_MAGENTA_RL_TUNER_NOTE_RNN_H_

#include <string>
#include <memory>
#include <vector>

#include "tensorflow/cc/client/client_session.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/cc/ops/dataset_ops_internal.h"
#include "tensorflow/cc/training/queue_runner.h"
#include "tensorflow/core/protobuf/queue_runner.pb.h"
#include "tensorflow/examples/cc/magenta/lstm/lstm_ops.h"
#include "tensorflow/cc/framework/gradients.h"

using namespace tensorflow::ops;
using namespace tensorflow::ops::internal;
using namespace std;

namespace tensorflow {

class NoteRNN {
 public:
  explicit NoteRNN(const ::tensorflow::Scope& s);
  ~NoteRNN();

  Status Init();

  // restore from frozen graph file
  Status Restore(const string path);
  Status UpdateState(const Tensor &h, const Tensor &c);

  operator ::tensorflow::Output() const { return logits; }
  operator ::tensorflow::Input() const { return logits; }

 public:
  Output w;
  Output b;
  Output w_y;
  Output b_y;
  Output cs;
  Output ada_w;
  Output ada_b;
  Output ada_w_y;
  Output ada_b_y;

  Output x;
  Output y;
  Output cs_prev;
  Output h_prev;

  Tensor y_tensor;
  Tensor h_prev_tensor;
  Tensor cs_prev_tensor;

  std::shared_ptr<BlockLSTM> block_lstm;

 private:
  const Scope& scope;
  const ClientSession &session;

  std::shared_ptr<RNNSoftmaxLoss> rnn_softmax_loss;

  std::vector<Output> grad_outputs;

  Output logits;


 private:
  Status BuildGraph();
};

}  // namespace tensorflow

#endif  // TENSORFLOW_EXAMPLES_CC_MAGENTA_RL_TUNER_NOTE_RNN_H_
