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

//
// C++ implementation of Magenta melody basic_rnn with LSTM,
// This is AutoGrad version;
//
// Author: Rock Zhuang
// Date  : May 30, 2019
// 

#include <cassert>

#include "tensorflow/cc/client/client_session.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/cc/ops/dataset_ops_internal.h"
#include "tensorflow/cc/training/queue_runner.h"
#include "tensorflow/core/protobuf/queue_runner.pb.h"
#include "lstm/lstm_ops.h"
#include "tensorflow/cc/framework/gradients.h"

using namespace tensorflow;
using namespace tensorflow::ops;
using namespace tensorflow::ops::internal;
using namespace std;

// #define VERBOSE 1
// #define TESTING 1

// Adjustable parameters
#define NUM_UNIT 128             // HIDDEN_SIZE
#define TIME_LEN 384             // NUM_STEPS
#define BATCH_SIZE 10            // 
#define TRAINING_STEPS 10000

// Don't change
#define INPUT_SIZE 38            // (DEFAULT_MAX_NOTE(84) - DEFAULT_MIN_NOTE(48) + NUM_SPECIAL_MELODY_EVENTS(2))
#define SEQ_LENGTH TIME_LEN * BATCH_SIZE

// #define LIBRARY_FILENAME "/home/rock/.cache/bazel/_bazel_rock/9982590d8d227cddee8c85cf45e44b89/execroot/org_tensorflow/bazel-out/k8-opt/bin/tensorflow/contrib/rnn/python/ops/_lstm_ops.so"
#define LIBRARY_FILENAME "/../../../../../../tensorflow/contrib/rnn/python/ops/_lstm_ops.so"

namespace tensorflow {
// Helpers for loading a TensorFlow plugin (a .so file).
Status LoadLibrary(const char* library_filename, void** result,
                   const void** buf, size_t* len);

std::string get_working_path()
{
   char temp[MAXPATHLEN];
   return (getcwd(temp, sizeof(temp)) ? std::string(temp) : std::string(""));
}

class InternalClientSession {
public:
  static tensorflow::Session* GetSession(tensorflow::ClientSession& session) {
    return session.GetSession();
  }
};

}

QueueRunnerDef BuildQueueRunnerDef(
    const std::string& queue_name, const std::vector<std::string>& enqueue_ops,
    const std::string& close_op, const std::string& cancel_op,
    const std::vector<tensorflow::error::Code>& queue_closed_error_codes) {
  QueueRunnerDef queue_runner_def;
  *queue_runner_def.mutable_queue_name() = queue_name;
  for (const std::string& enqueue_op : enqueue_ops) {
    *queue_runner_def.mutable_enqueue_op_name()->Add() = enqueue_op;
  }
  *queue_runner_def.mutable_close_op_name() = close_op;
  *queue_runner_def.mutable_cancel_op_name() = cancel_op;
  for (const auto& error_code : queue_closed_error_codes) {
    *queue_runner_def.mutable_queue_closed_exception_types()->Add() =
        error_code;
  }
  return queue_runner_def;
}

#ifdef TESTING

// Constructs a flat tensor with 'vals'.
template <typename T>
Tensor AsTensor(gtl::ArraySlice<T> vals) {
  Tensor ret(DataTypeToEnum<T>::value, {static_cast<int64>(vals.size())});
  std::copy_n(vals.data(), vals.size(), ret.flat<T>().data());
  return ret;
}

// Constructs a tensor of "shape" with values "vals".
template <typename T>
Tensor AsTensor(gtl::ArraySlice<T> vals, const TensorShape& shape) {
  Tensor ret;
  CHECK(ret.CopyFrom(AsTensor(vals), shape));
  return ret;
}

#define printTensor(T, d, DType) \
    std::cout<< (T).tensor<DType, (d)>() << std::endl

void test() {
  Tensor x = AsTensor<int>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
  TensorShape({6, 2}));

  Tensor xx(DT_INT32, TensorShape({2, 3, 2}));
  CHECK(xx.CopyFrom(x, TensorShape({2, 3, 2})));

  Tensor xxx = xx.SubSlice(0);
  Tensor x2 = x.Slice(0, 3);

  LOG(INFO) << __FUNCTION__ << "----------------------------x:" << endl << x.DebugString();
  printTensor(x, 2, int);
  LOG(INFO) << __FUNCTION__ << "----------------------------xx:" << endl << xx.DebugString();
  printTensor(xx, 3, int);
  LOG(INFO) << __FUNCTION__ << "----------------------------xxx:" << endl << xxx.DebugString();
  printTensor(xxx, 2, int);
  LOG(INFO) << __FUNCTION__ << "----------------------------x2:" << endl << x2.DebugString();
  printTensor(x2, 2, int);

  LOG(INFO) << __FUNCTION__ << "----------------------------current_path:" << endl << get_working_path();
}
#endif

int main() { 
#ifdef TESTING
  test();
#endif

  // // load lstm_ops library
  // void* unused_filehandle;
  // const void* buf;
  // size_t length;
  // TF_CHECK_OK(tensorflow::LoadLibrary(LIBRARY_FILENAME, &unused_filehandle, &buf, &length));
  std::string path = get_working_path();
  void* unused_filehandle;
  // TF_CHECK_OK(Env::Default()->LoadLibrary(LIBRARY_FILENAME, &unused_filehandle));
  TF_CHECK_OK(Env::Default()->LoadLibrary(path.append(LIBRARY_FILENAME).c_str(), &unused_filehandle));

  // Scope
  Scope root = Scope::NewRootScope();

  //
  // Dataset parsing
  //

  //
  // The file 'training_melodies.tfrecord' was generated by running the following commands from Magenta project: (put midi files into the folder '/tmp/data/midi/' in advance)
  //   $ convert_dir_to_note_sequences --input_dir=/tmp/data/midi/ --output_file=/tmp/data/notesequences.tfrecord --recursive
  //   $ melody_rnn_create_dataset --config=basic_rnn --input=/tmp/data/notesequences.tfrecord --output_dir=/tmp/data/ --eval_ratio=0.0
  //

  // TFRecordDataset
  Tensor filenames("/tmp/data/training_melodies.tfrecord");
  Tensor compression_type("");
  Tensor buffer_size((int64)1024);
  
  Output tfrecord_dataset = TFRecordDataset(root, filenames, compression_type, buffer_size);
  auto shuffle_and_repeat_dataset = ShuffleAndRepeatDataset(root, tfrecord_dataset, 
                        Cast(root, 5, DT_INT64),                                          // buffer_size
                        Cast(root, 0, DT_INT64), Cast(root, 0, DT_INT64),                 // seedX
                        Cast(root, 10, DT_INT64),                                         // count, -1 for infinite repetition
                        std::initializer_list<DataType>{DT_STRING}, 
                        std::initializer_list<PartialTensorShape>{{}});
  Output iterator_output = Iterator(root, "iterator1", "", vector<DataType>({DT_STRING}), vector<PartialTensorShape>({{}}));
  Operation make_iterator_op = MakeIterator(root, shuffle_and_repeat_dataset, iterator_output);
  auto iterator_get_next = IteratorGetNext(root, iterator_output, vector<DataType>({DT_STRING}), vector<PartialTensorShape>({{}}));

  // Input for ParseExample  
  Tensor feature_list_dense_missing_assumed_empty(DT_STRING, TensorShape({0}));

  vector<Output> feature_list_sparse_keys, feature_list_sparse_types,
                  feature_list_dense_keys, feature_list_dense_defaults;

  DataTypeSlice feature_list_dense_types = {DT_INT64, DT_FLOAT};
  gtl::ArraySlice<PartialTensorShape> feature_list_dense_shapes = {{}, {INPUT_SIZE}};

  vector<Output> context_sparse_keys, context_sparse_types, context_dense_keys,
                  context_dense_types, context_dense_defaults, context_dense_shapes;

  feature_list_dense_keys.push_back(Const<string>(root, "labels", TensorShape({})));
  feature_list_dense_keys.push_back(Const<string>(root, "inputs", TensorShape({})));

  feature_list_dense_defaults.push_back(Const<int64>(root, 2, TensorShape({})));
  feature_list_dense_defaults.push_back(Const<float>(root, 1, TensorShape({INPUT_SIZE})));

  // ParseSingleSequenceExample parse only one sequence. ParseSequenceExample supports batch inputs
  auto parse_single_sequence_example = ParseSingleSequenceExample(root, iterator_get_next[0], 
                            feature_list_dense_missing_assumed_empty,
                            InputList(context_sparse_keys),
                            InputList(context_dense_keys),
                            InputList(feature_list_sparse_keys),
                            InputList(feature_list_dense_keys),
                            InputList(context_dense_defaults),
                            Const<string>(root, "melody_rnn_training sequence parsing", TensorShape({})),
                            ParseSingleSequenceExample::Attrs().FeatureListDenseTypes(feature_list_dense_types).FeatureListDenseShapes(feature_list_dense_shapes));

  // QueueRunner 
  constexpr char kCancelOp[] = "cancel0";
  constexpr char kCloseOp[] = "close0";
  constexpr char kDequeueOp[] = "dequeue0";
  constexpr char kDequeueOp1[] = "dequeue0:1";
  constexpr char kEnqueueOp[] = "enqueue0";
  constexpr char kQueueName[] = "fifoqueue";

  auto pfq = FIFOQueue(root.WithOpName(kQueueName), {DataType::DT_INT64, DataType::DT_FLOAT});
  auto enqueue = QueueEnqueue(root.WithOpName(kEnqueueOp), pfq, InputList(parse_single_sequence_example.feature_list_dense_values));
  auto closequeue = QueueClose(root.WithOpName(kCloseOp), pfq);
  auto cancelqueue = QueueClose(root.WithOpName(kCancelOp), pfq,
                            QueueClose::CancelPendingEnqueues(true));
  // QueueDequeueMany to deque multiple items as a batch
  auto dequeue = QueueDequeue(root.WithOpName(kDequeueOp), pfq, {DataType::DT_INT64, DataType::DT_FLOAT});

  // Session
  // Note that ClientSession can extend graph before running, Session cannot.
  vector<Tensor> dataset_outputs;
  ClientSession session(root);

  // Run make_iterator_output first
  TF_CHECK_OK(session.Run({}, {}, {make_iterator_op}, nullptr));
  
  // Coordinator and QueueRunner
  QueueRunnerDef queue_runner =
      BuildQueueRunnerDef(kQueueName, {kEnqueueOp}, kCloseOp, kCancelOp,
                          {tensorflow::error::Code::OUT_OF_RANGE, tensorflow::error::Code::CANCELLED});  
  Coordinator coord;
  std::unique_ptr<QueueRunner> qr;
  TF_CHECK_OK(QueueRunner::New(queue_runner, &coord, &qr));
  TF_CHECK_OK(qr->Start(InternalClientSession::GetSession(session)));
  TF_CHECK_OK(coord.RegisterRunner(std::move(qr)));

  while(session.Run(RunOptions(), {}, {kDequeueOp, kDequeueOp1}, {}, &dataset_outputs).ok()) {
#ifdef VERBOSE
    LOG(INFO) << "Print deque: " << dataset_outputs[0].DebugString() << ", " << dataset_outputs[1].DebugString();    
    // for(int i = 0; i < dataset_outputs[0].NumElements(); i++) {
    //   LOG(INFO) << "Print labels: " << dataset_outputs[0].vec<int64>()(i);
    // }
#endif
  }
  // For now, only the lastest sequence example is used below

  //
  // Train
  //
  
  // Trainable parameters
  auto w = Variable(root.WithOpName("w"), {INPUT_SIZE + NUM_UNIT, NUM_UNIT * 4}, DT_FLOAT); // (input_size + cell_size, cell_size * 4) for {i, c, f, o}
  auto rate = Const(root, {0.01f});
  auto random_value = RandomNormal(root, {INPUT_SIZE + NUM_UNIT, NUM_UNIT * 4}, DT_FLOAT);
  auto assign_w = Assign(root, w, Multiply(root, random_value, rate));

  auto b = Variable(root.WithOpName("b"), {NUM_UNIT * 4}, DT_FLOAT);
  Tensor b_zero_tensor(DT_FLOAT, TensorShape({NUM_UNIT * 4}));
  b_zero_tensor.vec<float>().setZero();  
  auto assign_b = Assign(root, b, ZerosLike(root, b_zero_tensor));

  // used to compute dh (h_grad)
  auto w_y = Variable(root.WithOpName("w_y"), {INPUT_SIZE, NUM_UNIT}, DT_FLOAT); 
  auto random_value2 = RandomNormal(root, {INPUT_SIZE, NUM_UNIT}, DT_FLOAT);
  auto assign_w_y = Assign(root, w_y, Multiply(root, random_value2, rate));

  auto b_y = Variable(root.WithOpName("b_y"), {INPUT_SIZE}, DT_FLOAT);
  Tensor b_y_zero_tensor(DT_FLOAT, TensorShape({INPUT_SIZE}));
  b_y_zero_tensor.vec<float>().setZero();  
  auto assign_b_y = Assign(root, b_y, ZerosLike(root, b_y_zero_tensor));

  auto cs = Variable(root.WithOpName("cs"), {TIME_LEN, BATCH_SIZE, NUM_UNIT}, DT_FLOAT);
  Tensor cs_zero_tensor(DT_FLOAT, TensorShape({TIME_LEN, BATCH_SIZE, NUM_UNIT}));
  cs_zero_tensor.tensor<float, 3>().setZero();  
  auto assign_cs = Assign(root, cs, ZerosLike(root, cs_zero_tensor));

  // Gradient accum parameters start here
  auto ada_w = Variable(root, {INPUT_SIZE + NUM_UNIT, NUM_UNIT * 4}, DT_FLOAT);
  auto assign_ada_w = Assign(root, ada_w, ZerosLike(root, w));

  auto ada_b = Variable(root, {NUM_UNIT * 4}, DT_FLOAT);
  auto assign_ada_b = Assign(root, ada_b, ZerosLike(root, b));

  auto ada_w_y = Variable(root, {INPUT_SIZE, NUM_UNIT}, DT_FLOAT);
  auto assign_ada_w_y = Assign(root, ada_w_y, ZerosLike(root, w_y));

  auto ada_b_y = Variable(root, {INPUT_SIZE}, DT_FLOAT);
  auto assign_ada_b_y = Assign(root, ada_b_y, ZerosLike(root, b_y));

  // Placeholders
  auto x = Placeholder(root, DT_FLOAT, Placeholder::Shape({TIME_LEN, BATCH_SIZE, INPUT_SIZE})); // (timelen, batch_size, num_inputs)
  auto y = Placeholder(root, DT_INT64, Placeholder::Shape({TIME_LEN, BATCH_SIZE})); // (timelen, batch_size)

  auto cs_prev = Placeholder(root, DT_FLOAT, Placeholder::Shape({BATCH_SIZE, NUM_UNIT})); // (batch_size, cell_size)
  auto h_prev = Placeholder(root, DT_FLOAT, Placeholder::Shape({BATCH_SIZE, NUM_UNIT}));

  // LSTM
  auto block_lstm = BlockLSTM(root, 
                              Const<int64>(root, {TIME_LEN}), // seq_len_max,
                              x,
                              cs_prev,
                              h_prev, 
                              w,
                              Const<float>(root, 0, TensorShape({NUM_UNIT})), // wci, used when use_peephole is true
                              Const<float>(root, 0, TensorShape({NUM_UNIT})), // wcf
                              Const<float>(root, 0, TensorShape({NUM_UNIT})), // wco
                              b);
  LOG(INFO) << "Node building status: " << root.status();
  
  auto rnn_softmax_loss = RNNSoftmaxLoss(root, 
                              block_lstm.h,
                              y,
                              w_y,
                              b_y,
                              cs);
  LOG(INFO) << "Node building status: " << root.status();

  // Gradients
  std::vector<Output> grad_outputs;
  TF_CHECK_OK(AddSymbolicGradients(root, {rnn_softmax_loss.loss}, 
                                   {w, b, w_y, b_y}, 
                                   &grad_outputs));
  LOG(INFO) << "Node building status: " << root.status();

  // Gradient
  auto lr = Cast(root, 0.03, DT_FLOAT);

  // alternative of ApplyAdagrad
  auto apply_w = ApplyAdagradTrick(root, w, ada_w, lr, grad_outputs[0]);
  LOG(INFO) << "Node building status: " << root.status();
  auto apply_b = ApplyAdagradTrick(root, b, ada_b, lr, grad_outputs[1]);
  LOG(INFO) << "Node building status: " << root.status();
  auto apply_w_y = ApplyAdagradTrick(root, w_y, ada_w_y, lr, grad_outputs[2]);
  LOG(INFO) << "Node building status: " << root.status();
  auto apply_b_y = ApplyAdagradTrick(root, b_y, ada_b_y, lr, grad_outputs[3]);
  LOG(INFO) << "Node building status: " << root.status();

  // Initialize variables
  TF_CHECK_OK(session.Run({assign_w, assign_b, assign_w_y, assign_b_y, assign_cs}, 
                          nullptr));
  TF_CHECK_OK(session.Run({assign_ada_w, assign_ada_b, assign_ada_w_y, assign_ada_b_y}, 
                          nullptr));

  // loop
  int step = 0;
  while(step < TRAINING_STEPS) {

    // zeroed out when batch 0
    Tensor h_prev_tensor(DT_FLOAT, TensorShape({BATCH_SIZE, NUM_UNIT}));
    typename TTypes<float>::Matrix h_prev_t = h_prev_tensor.matrix<float>();
    h_prev_t.setZero();

    Tensor cs_prev_tensor(DT_FLOAT, TensorShape({BATCH_SIZE, NUM_UNIT}));
    typename TTypes<float>::Matrix cs_prev_t = cs_prev_tensor.matrix<float>();
    cs_prev_t.setZero();

    // Train
    {
      // Note that every input batch in BATCH_SIZE is from a different example
      Tensor x_tensor(DT_FLOAT, TensorShape({TIME_LEN, BATCH_SIZE, INPUT_SIZE}));
      {
        auto e_2d = x_tensor.shaped<float, 2>({SEQ_LENGTH, INPUT_SIZE});

        for (int i = 0; i < TIME_LEN; i++) {
          Eigen::DSizes<Eigen::DenseIndex, 2> indices_dataset(i, 0);
          Eigen::DSizes<Eigen::DenseIndex, 2> sizes_dataset(1, INPUT_SIZE);
          Eigen::Tensor<float, 2, Eigen::RowMajor> mat = dataset_outputs[1].matrix<float>().slice(indices_dataset, sizes_dataset);

          for(int b = 0; b < BATCH_SIZE; b++) {            
            // set e_2d
            Eigen::DSizes<Eigen::DenseIndex, 2> indices(i * BATCH_SIZE + b, 0);
            Eigen::DSizes<Eigen::DenseIndex, 2> sizes(1, INPUT_SIZE);
            e_2d.slice(indices, sizes) = mat;
          }
        }
      }

      // y
      Tensor y_tensor(DT_INT64, TensorShape({TIME_LEN, BATCH_SIZE}));
      {
        typename TTypes<int64>::Vec y_t = y_tensor.shaped<int64, 1>({SEQ_LENGTH});

        // Prepare y
        for (int i = 0; i < TIME_LEN; i++) {
          int64 label = dataset_outputs[0].vec<int64>()(i + 1); // i + 1 for the next one

          for(int b = 0; b < BATCH_SIZE; b++) {
            y_t(i * BATCH_SIZE + b) = label;
          }
        }
      }

      // Run 
      vector<Tensor> outputs;
      TF_CHECK_OK(session.Run({{x, x_tensor}, {y, y_tensor}, {h_prev, h_prev_tensor}, {cs_prev, cs_prev_tensor}}, 
                              {rnn_softmax_loss.loss,  
                                              apply_w, apply_b, apply_w_y, apply_b_y}, 
                              {}, 
                              &outputs));

      if(step % 100 == 0) {
#ifdef VERBOSE  
        LOG(INFO) << "Print step: " << step << ", loss: " << outputs[0].DebugString();
#endif
        Eigen::Tensor<float, 0, Eigen::RowMajor> total_loss = outputs[0].flat<float>().sum();
        LOG(INFO) << "Print step: " << step << ", total_loss: " << total_loss();
      }
    }

    step++;
  }

  // Update h_prev_tensor and cs_prev_tensor, cs_grad_tensor for the next BATCH

  // Stop
  TF_CHECK_OK(coord.RequestStop());
  TF_CHECK_OK(coord.Join());

  return 0;
}