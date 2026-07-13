from abc import ABCMeta, abstractmethod

import lldb


class ScriptedStackFrameRecognizer(metaclass=ABCMeta):
    """
    The base class for a scripted stack frame recognizer.

    A frame recognizer allows you to provide extra information about a stack
    frame, such as recognized arguments, used by commands like `bt`. Register
    it with `frame recognizer add -l <ClassName> ...`.

    Most of the base class methods are `@abstractmethod` that need to be
    overwritten by the inheriting class.
    """

    def __init__(self):
        """Construct a scripted stack frame recognizer.

        Recognizers are constructed with no arguments and are shared across
        every frame they're asked to recognize.
        """
        pass

    @abstractmethod
    def get_recognized_arguments(self, frame):
        """Get the arguments recognized for this frame.

        Args:
            frame (lldb.SBFrame): The frame to inspect.

        Returns:
            list of lldb.SBValue: The recognized arguments, or an empty list
            if none could be recognized.
        """
        pass

    def should_hide(self, frame):
        """Whether this frame should be hidden when displaying backtraces.

        Args:
            frame (lldb.SBFrame): The frame to inspect.

        Returns:
            bool: `True` if this frame should be hidden, `False` otherwise.
            Defaults to `False`.
        """
        return False
