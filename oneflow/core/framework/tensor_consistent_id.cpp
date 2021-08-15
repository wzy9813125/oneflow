/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "oneflow/core/framework/tensor.h"
#include "oneflow/core/framework/tensor_tuple.h"
#include "oneflow/core/framework/transport_token.h"
#include "oneflow/core/framework/tensor_consistent_id.h"

namespace oneflow {

namespace one {

int64_t* MutThreadLocalRecursiveDepth() {
  static thread_local int64_t recursive_depth = 0;
  return &recursive_depth;
}

Maybe<void> InitConsistentId(TensorTuple* outputs) {
  for (int i = 0; i < outputs->size(); ++i) {
    const auto& consistent_tensor = std::dynamic_pointer_cast<ConsistentTensor>(outputs->at(i));
    CHECK_OR_RETURN(consistent_tensor)
        << Error::Unimplemented() << "consistent tensors suppported only.";
    const auto& transport_token = JUST(TransportToken::NewMetaTransportToken());
    JUST(consistent_tensor->mut_impl()->set_transport_token(transport_token));
  }
  return Maybe<void>::Ok();
}

}  // namespace one

}  // namespace oneflow
