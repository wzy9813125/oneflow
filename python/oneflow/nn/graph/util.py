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
import sys
from collections import OrderedDict

from oneflow.framework.tensor import Tensor
from typing import Callable, Dict, Union, List, Tuple


def add_indent(in_s, num_spaces):
    s = in_s.split("\n")
    if len(s) == 1:
        return in_s
    first = s.pop(0)
    s = [num_spaces * " " + line for line in s]
    s = "\n".join(s)
    s = first + "\n" + s
    return s


def sys_exc_error_msg():
    msg = ""
    exc_info = sys.exc_info()
    if len(exc_info) > 0:
        msg += str(exc_info[0])
    if len(exc_info) > 1:
        msg += " " + str(exc_info[1])
    return msg


def seq_to_func_return(seq, need_unpack=False):
    if need_unpack:
        return seq[0]
    return seq


class NamedIONode(object):
    def __init__(self, prefix="", name=None, global_index=0, local_index=0) -> None:
        self._name = name if name is not None else str(global_index)
        self._prefix = prefix
        self._global_index = global_index
        self._local_index = local_index
        self._is_value_set = False
        self._value = None

    def prefix(self):
        return self._prefix

    def name(self):
        return self._name

    def local_index(self):
        return self._local_index

    def global_index(self):
        return self._global_index

    def value(self):
        assert self._is_value_set, "self._value is not set yet"
        return self._value

    def is_leaf(self):
        assert self._is_value_set, "self._value is not set yet"
        return not (
            isinstance(self._value, dict)
            or isinstance(self._value, tuple)
            or isinstance(self._value, list)
        )

    def set_value(self, value):
        assert not isinstance(
            value, NamedIONode
        ), "IONode cannot accept value of type NamedIONode"
        self._value = value
        self._is_value_set = True

    def named_nodes(self, memo=None):
        assert self._is_value_set, "self._value is not set yet"
        queue = [self]

        while len(queue) > 0:
            curr = queue.pop(0)
            if isinstance(curr, NamedIONode):
                queue.append(curr.value())
                yield (curr.prefix() + "_" + curr.name(), curr)
            elif isinstance(curr, tuple) or isinstance(curr, list):
                for v in curr:
                    queue.append(v)
            elif isinstance(curr, dict):
                for v in curr.values():
                    queue.append(v)

    def __repr__(self):
        repr_str = ""
        repr_str += "(name: " + self._name
        repr_str += ", idx: " + str(self._global_index)
        repr_str += ", type: "
        if isinstance(self._value, tuple):
            repr_str += "TUPLE"
        elif isinstance(self._value, list):
            repr_str += "LIST"
        elif isinstance(self._value, dict):
            repr_str += "DICT"
        elif isinstance(self._value, Tensor):
            repr_str += "TENSOR"
        elif self._value is None:
            repr_str += "NONE"
        else:
            repr_str += "OPAQUE"
        if isinstance(self._value, Tensor):
            repr_str += ", value: " + self._value._meta_repr()
        elif (
            isinstance(self._value, dict)
            or isinstance(self._value, list)
            or isinstance(self._value, tuple)
        ):
            pass
        else:
            repr_str += ", value: " + repr(self._value)
        repr_str += ")"
        return repr_str


def construct_io_node(value, root_prefix: str, root_name: str) -> NamedIONode:
    global_index = 0

    def construct(value, prefix: str, name: str, local_index: int) -> NamedIONode:
        nonlocal global_index
        node = NamedIONode(prefix, name, global_index, local_index)
        global_index += 1
        if isinstance(value, list) or isinstance(value, tuple):

            def construct_func(enum):
                (i, v) = enum
                next_prefix = prefix + ("." if prefix else "") + str(i)
                new_node = construct(v, next_prefix, None, i)
                return new_node

            node.set_value(value.__class__(map(construct_func, enumerate(value))))

        elif isinstance(value, dict):

            def construct_func(enum):
                i, (key, v) = enum
                next_prefix = prefix + ("." if prefix else "") + str(i)
                new_node = construct(v, next_prefix, key, i)
                return key, new_node

            node.set_value(dict(map(construct_func, enumerate(value.items()))))
        else:
            node.set_value(value)
        return node

    root_node = construct(value, root_prefix, root_name, 0)
    return root_node


def map_io(io_values, map_function: Callable):
    assert (
        isinstance(io_values, dict)
        or isinstance(io_values, tuple)
        or isinstance(io_values, list)
        or isinstance(io_values, NamedIONode)
    ), "must be one of those types"

    assert map_function != None, "map function cannot be None"

    def execute_mapping(value):
        if isinstance(value, tuple) or isinstance(value, list):
            mapped_value = value.__class__(
                map(lambda x: execute_mapping(x), value)
            )
        elif isinstance(value, dict):
            mapped_value = dict(
                map(lambda x: (x[0], execute_mapping(x[1])), value.items())
            )
        elif isinstance(value, NamedIONode):
            assert False
        else:
            mapped_value = map_function(value)

        return mapped_value
    return execute_mapping(io_values)

class IOMapper(object):
    def __init__(self, io_values, map_function: Callable) -> None:
        assert io_values != None
        assert map_function != None

        self._io_values = io_values
        self._mapped_io_values = None
        self._map_function = map_function

    def get_mapping_result(self):
        if self.is_mapped():
            return self._mapped_io_values

        def execute_mapping(value, key_or_index=None):

            if isinstance(value, tuple) or isinstance(value, list):
                mapped_value = value.__class__(
                    map(lambda x: execute_mapping(x[1], x[0]), enumerate(value))
                )
            elif isinstance(value, dict):
                mapped_value = dict(
                    map(lambda x: (x[0], execute_mapping(x[1], x[0])), value.items())
                )
            elif isinstance(value, NamedIONode):
                assert False
            else:
                mapped_value = self._map_function(value)

            return mapped_value

        self._mapped_io_values = execute_mapping(self._io_values)
        return self._mapped_io_values


class IONodeType:
    TENSOR = "TENSOR"
    NONE = "NONE"
    LIST = "LIST"
    TUPLE = "TUPLE"
    DICT = "DICT"
    OPAQUE = "OPAQUE"


class IONode(object):
    def __init__(self, name=None, start_idx=0, value=None, prefix=""):
        # Node indexs
        self._name = name if name is not None else str(start_idx)
        self._prefix = prefix
        self._start_idx = start_idx
        self._end_idx = start_idx
        self._cur_level_idx = -1
        self._sub_nodes = OrderedDict()
        self.attrs = dict()
        self._is_leaf = False

        if isinstance(value, tuple):
            self._type = IONodeType.TUPLE
            self._value = None
            for idx, item in enumerate(value):
                subnode_prefix = (
                    self._prefix
                    + ("." if self._prefix else "")
                    + str(self._cur_level_idx + 1)
                )
                self.__add_sub_node(
                    IONode(None, self._end_idx + 1, item, subnode_prefix)
                )
        elif isinstance(value, list):
            self._type = IONodeType.LIST
            self._value = None
            for idx, item in enumerate(value):
                subnode_prefix = (
                    self._prefix
                    + ("." if self._prefix else "")
                    + str(self._cur_level_idx + 1)
                )
                self.__add_sub_node(
                    IONode(None, self._end_idx + 1, item, subnode_prefix)
                )
        elif isinstance(value, dict):
            self._type = IONodeType.DICT
            self._value = None
            for idx, (key, item) in enumerate(value.items()):
                subnode_prefix = (
                    self._prefix
                    + ("." if self._prefix else "")
                    + str(self._cur_level_idx + 1)
                )
                self.__add_sub_node(
                    IONode(key, self._end_idx + 1, item, subnode_prefix)
                )
        elif isinstance(value, Tensor):
            self._type = IONodeType.TENSOR
            self._value = value
            self._is_leaf = True
        elif value is None:
            self._type = IONodeType.NONE
            self._value = value
            self._is_leaf = True
        else:
            self._type = IONodeType.OPAQUE
            self._value = value
            self._is_leaf = True

    def size(self):
        return self._end_idx - self._start_idx + 1

    def __add_sub_node(self, node):
        self._sub_nodes[self._cur_level_idx + 1] = node
        self._end_idx += node.size()
        self._cur_level_idx += 1

    def named_nodes(self, memo=None):
        if memo is None:
            memo = set()
        if self not in memo:
            memo.add(self)
            yield (self._prefix + "_" + str(self._name), self)
            for (level_idx, node) in self._sub_nodes.items():
                if node is None:
                    continue
                for n in node.named_nodes(memo):
                    yield n

    def __repr__(self):
        repr_str = ""
        repr_str += "(name: " + self._name
        repr_str += ", idx: " + str(self._start_idx)
        repr_str += ", type: " + self._type
        if self._type == IONodeType.TENSOR:
            repr_str += ", value: " + self._value._meta_repr() + ")"
        else:
            repr_str += ", value: " + repr(self._value) + ")"
        return repr_str

    def map_leaf(self, leaf_node_fn):
        if self._type == IONodeType.TUPLE:
            l_value = list()
            for (name, node) in self._sub_nodes.items():
                l_value.append(node.map_leaf(leaf_node_fn))
            mapped_value = tuple(l_value)
        elif self._type == IONodeType.LIST:
            mapped_value = list()
            for (name, node) in self._sub_nodes.items():
                mapped_value.append(node.map_leaf(leaf_node_fn))
        elif self._type == IONodeType.DICT:
            mapped_value = dict()
            for (name, node) in self._sub_nodes.items():
                mapped_value[node._name] = node.map_leaf(leaf_node_fn)
        else:
            # Leaf node: TENSOR/NONE/OPAQUE
            mapped_value = leaf_node_fn(self)
        return mapped_value
