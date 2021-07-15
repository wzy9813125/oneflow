"""
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
"""
from __future__ import absolute_import

import oneflow.python.framework.c_api_util as c_api_util
import oneflow.python.framework.hob as hob
import oneflow.python.eager.gradient_util as gradient_util
import oneflow.python.lib.core.enable_if as enable_if
from oneflow.python.oneflow_export import oneflow_export
import oneflow.python.framework.remote_blob as remote_blob_util
import oneflow._oneflow_internal


@oneflow_export("losses.add_loss")
def api_add_loss(loss: oneflow._oneflow_internal.BlobDesc) -> None:
    r"""Mark a `Blob` as a loss. Auto grad starts at every loss blob. It doesn't has to be a product of typical "loss" operator like softmax loss but can also be a `Blob` produced by any operator.

    Args:
        loss: A `Blob`.
    """
    return lazy_add_loss(loss) # NOTE(chengcheng): global_function ONLY support Lazy run.


@enable_if.condition(
    hob.in_global_mode & hob.is_trainable
)
def lazy_add_loss(loss):
    c_api_util.CurJobBuildAndInferCtx_AddLossLogicalBlobName(loss.unique_name)


@enable_if.condition(
    hob.in_global_mode & hob.is_trainable)
def eager_add_loss(loss):
    c_api_util.CurJobBuildAndInferCtx_AddLossLogicalBlobName(loss.unique_name)
    gradient_util.GetDefaultBackwardBlobRegister().TrySetObject4BlobName(
        loss.logical_blob_name, loss.blob_object
    )
