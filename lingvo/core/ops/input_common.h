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

#ifndef LINGVO_CORE_OPS_INPUT_COMMON_H_
#define LINGVO_CORE_OPS_INPUT_COMMON_H_

#include <limits>
#include "tensorflow/core/framework/op_kernel.h"
#include "lingvo/core/ops/record_batcher.h"
#include "lingvo/core/ops/record_yielder.h"
#include "lingvo/core/ops/weighted_mix_record_yielder.h"

namespace tensorflow {
namespace lingvo {

// Base class for op kernels that emit training examples.
template <class RecordProcessorClass>
class InputOp : public OpKernel {
  static_assert(
      std::is_base_of<RecordProcessor, RecordProcessorClass>::value,
      "InputOp requires a RecordProcessor subclass as the template arg.");

 public:
  explicit InputOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    typedef std::vector<int64> Int64Vec;
#define GETATTR(TYPE, FIELD) \
  TYPE FIELD;                \
  OP_REQUIRES_OK(ctx, ctx->GetAttr(#FIELD, &FIELD));

    GETATTR(string, file_pattern);
    GETATTR(std::vector<float>, input_source_weights);
    GETATTR(int64, file_random_seed);
    GETATTR(int64, file_buffer_size);
    GETATTR(int64, file_parallelism);
    GETATTR(Int64Vec, bucket_upper_bound);
    GETATTR(Int64Vec, bucket_batch_limit);
    GETATTR(int64, flush_every_n);
    GETATTR(int64, num_threads);
#undef GETATTR
    OP_REQUIRES(
        ctx,
        std::is_sorted(bucket_upper_bound.begin(), bucket_upper_bound.end()),
        errors::InvalidArgument("Bucket_upper_bound is not sorted"));
    LOG(INFO) << "Create RecordProcessor";
    processor_ = new RecordProcessorClass(ctx);

    std::vector<string> file_patterns;
    if (input_source_weights.empty()) {
      LOG(INFO) << "Input source weights are empty, fall back to legacy "
                << "behavior.";
      file_patterns.push_back(file_pattern);
    } else {
      file_patterns  = str_util::Split(file_pattern, ',');
      OP_REQUIRES(
        ctx,
        file_patterns.size() == input_source_weights.size(),
        errors::InvalidArgument("There should be exactly one "
                                "input_source_weight per coma-separated value "
                                "in file_pattern."));
    }
    std::vector<RecordYielder*> yielders;
    for (int i = 0; i < file_patterns.size(); ++i) {
      BasicRecordYielder::Options yopts;
      yopts.file_pattern = file_patterns[i];
      if (file_random_seed == 0) {
        yopts.seed = 0;  // Let the yielder pick a random seed.
      } else {
        yopts.seed =
            (file_random_seed + i) % (std::numeric_limits<int32>::max() - 1);
        if (yopts.seed == 0) {
          // Add 1 to avoid 0.
          ++yopts.seed;
        }
      }
      yopts.bufsize = file_buffer_size;
      yopts.parallelism = file_parallelism;
      yielders.push_back(BasicRecordYielder::New(yopts));
    }
    RecordYielder* yielder = nullptr;
    if (yielders.size() > 1) {
      yielder = WeightedMixRecordYielder::New(file_random_seed, yielders,
                                              input_source_weights);
    } else {
      yielder = yielders.front();
    }

    LOG(INFO) << "Create batcher";
    RecordBatcher::Options bopts;
    bopts.bucket_upper_bound = bucket_upper_bound;
    bopts.bucket_batch_limit = bucket_batch_limit;
    bopts.flush_every_n = flush_every_n;
    bopts.num_threads = num_threads;
    batcher_ = new RecordBatcher(bopts, yielder, processor_);
  }

  ~InputOp() override { delete batcher_; }

  void Compute(OpKernelContext* ctx) override {
    int64 bucket_id;
    TensorVec batch;
    batcher_->GetNext(&bucket_id, &batch);
    VLOG(1) << "Produce a batch from bucket : " << bucket_id;
    OP_REQUIRES(ctx, static_cast<int>(batch.size()) == ctx->num_outputs(),
                errors::Internal("Unexpected batch: ", batch.size()));
    for (int i = 0; i < batch.size(); ++i) {
      ctx->set_output(i, batch[i]);
    }
  }

 protected:
  // Not owned.
  RecordProcessorClass* processor_ = nullptr;

 private:
  // Owned.
  RecordBatcher* batcher_ = nullptr;
};

}  // namespace lingvo
}  // namespace tensorflow

#endif  // LINGVO_CORE_OPS_INPUT_COMMON_H_
