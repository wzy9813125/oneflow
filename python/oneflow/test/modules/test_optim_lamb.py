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
import unittest
from collections import OrderedDict
import numpy as np

from test_util import GenArgList
from optimizer_test_util import clip_grad_norm_np

import oneflow as flow


def compare_with_numpy_lamb(
    test_case,
    device,
    x_shape,
    learning_rate,
    train_iters,
    betas,
    weight_decay,
    eps,
    do_bias_correction,
    amsgrad,
    clip_grad_args,
):
    clip_grad_max_norm, clip_grad_norm_type = clip_grad_args

    np.random.seed(1000)

    random_grad_seq = []
    for _ in range(train_iters):
        random_grad_seq.append(np.random.uniform(size=x_shape).astype(np.float32))
    init_value = np.random.uniform(size=x_shape).astype(np.float32)

    of_res_list = []

    def train_by_oneflow():
        x = flow.nn.Parameter(flow.Tensor(init_value, device=flow.device(device)))

        optim_kwargs = {
            "params": [x],
            "lr": learning_rate,
            "betas": betas,
            "eps": eps,
            "weight_decay": weight_decay,
            "do_bias_correction": do_bias_correction,
        }

        if clip_grad_max_norm != -1:
            optim_kwargs["clip_grad_max_norm"] = clip_grad_max_norm
            optim_kwargs["clip_grad_norm_type"] = clip_grad_norm_type

        lamb = flow.optim.LAMB([optim_kwargs])

        def train_one_iter(grad):
            grad_tensor = flow.tensor(
                grad,
                dtype=flow.float32,
                requires_grad=False,
                device=flow.device(device),
            )

            loss = flow.sum(x * grad_tensor)
            loss.backward()
            if clip_grad_max_norm != -1:
                lamb.clip_grad()
            lamb.step()
            lamb.zero_grad()
            return x

        for i in range(train_iters):
            param = train_one_iter(random_grad_seq[i])

            of_res_list.append(param.numpy())

    train_by_oneflow()

    np_res_list = []

    def train_by_numpy():
        x = init_value
        mt = np.zeros_like(x)
        vt = np.zeros_like(x)
        beta1 = betas[0]
        beta2 = betas[1]

        def np_train_one_iter(step, grad):
            if clip_grad_max_norm != -1:
                total_norm, grad = clip_grad_norm_np(
                    grad, clip_grad_max_norm, clip_grad_norm_type
                )

            # grad = grad + weight_decay * x
            bias_correction1 = 1.0
            bias_correction2 = 1.0

            if do_bias_correction:
                bias_correction1 = 1.0 - np.power(beta1, step)
                bias_correction2 = 1.0 - np.power(beta2, step)

            m = beta1 * mt + (1 - beta1) * grad
            v = beta2 * vt + (1 - beta2) * grad * grad

            # if amsgrad:
            #     denom = np.sqrt(max_s) / np.sqrt(bias_correction2) + eps
            # else:
            #     denom = np.sqrt(s) / np.sqrt(bias_correction2) + eps

            adam_diff = (m / (1 - beta1)) / (np.sqrt(v / (1 - beta2)) + eps)
            w_norm = np.linalg.norm(x, ord=2)
            g_norm = np.linalg.norm(adam_diff, ord=2)
            if w_norm > 0 and g_norm > 0:
                trust_ratio = w_norm / g_norm
            else:
                trust_ratio = 1.0

            param = x - learning_rate * trust_ratio * (adam_diff + weight_decay * x)
            return (param, m, v)

        for i in range(train_iters):
            (x, mt, vt) = np_train_one_iter(i, random_grad_seq[i])
            np_res_list.append(x)
        return x

    train_by_numpy()

    test_case.assertTrue(np.allclose(of_res_list, np_res_list, rtol=1e-3, atol=1e-3))
    # if not np.allclose(of_res_list, np_res_list, rtol=1e-3, atol=1e-3):
    # print("catch")


@flow.unittest.skip_unless_1n1d()
class TestLamb(flow.unittest.TestCase):
    def test_lamb(test_case):
        arg_dict = OrderedDict()
        arg_dict["device"] = ["cpu", "cuda"]
        arg_dict["x_shape"] = [(10,)]
        arg_dict["learning_rate"] = [0.1, 1e-3]
        arg_dict["train_iters"] = [10]
        arg_dict["betas"] = [(0.99, 0.9)]
        arg_dict["weight_decay"] = [0.001]
        arg_dict["eps"] = [1e-8, 1e-6]
        arg_dict["do_bias_correction"] = [False]
        arg_dict["amsgrad"] = [False]
        # NOTE(xyliao): max_norm == -1 means no clip grad
        arg_dict["clip_grad_args"] = [(-1, 2.0), (1, 2.0)]

        for arg in GenArgList(arg_dict):
            compare_with_numpy_lamb(test_case, *arg)


if __name__ == "__main__":
    unittest.main()
