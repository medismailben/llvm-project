"""Inferior for TestCPythonFrameProvider.

Run under lldb with a breakpoint on `getppid`, which CPython only reaches
here, so the stop always has this file's three functions on the Python stack.
"""

import os


def leaf(count, label):
    numbers = [1, 2, 3]
    ratio = 1.5
    flag = True
    nothing = None
    return os.getppid()


def middle(count):
    inner_label = "from-middle"
    return leaf(count, inner_label)


def outer():
    return middle(3)


outer()
