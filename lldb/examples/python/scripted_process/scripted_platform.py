from abc import ABCMeta, abstractmethod

import lldb

class ScriptedPlatform(metaclass=ABCMeta):

    """
    The base class for a scripted platform.

    Most of the base class methods are `@abstractmethod` that need to be
    overwritten by the inheriting class.

    DISCLAIMER: THIS INTERFACE IS STILL UNDER DEVELOPMENT AND NOT STABLE.
                THE METHODS EXPOSED MIGHT CHANGE IN THE FUTURE.
    """

    processes = None

    @abstractmethod
    def __init__(self, args):
        """ Construct a scripted platform.

        Args:
            args (lldb.SBStructuredData): A Dictionary holding arbitrary
                key/value pairs used by the scripted platform.
        """
        processes = []

    @abstractmethod
    def list_processes(self):
        """ Get a list of processes that can be ran on the platform.

        process_info = {
            name = a.out,
            arch = aarch64,
            pid = 420
            parent_pid = 42 (optional)
            uid = 0 (optional)
            gid = 0 (optional)
        }

        Returns:
            Dict: The processes represented as a dictionary, with at least the
                process ID, name, architecture. Optionally, the user can also
                provide the parent process ID and the user and group IDs.
                The dictionary can be empty.
        """
        pass

    def get_process_info(self, pid):
        """ Get the dictionary describing the process.

        Returns:
            Dict: The dictionary of process info that matched process ID.
            None if the process doesn't exists
        """
        pass

    @abstractmethod
    def launch_process(self, launch_info):
        """ Launch a scripted process.

        Args:
            launch_info (lldb.SBLaunchInfo): The information related to the process launch.

        Returns:
            lldb.SBError: A status object notifying if the launch succeeded.
        """
        pass

    @abstractmethod
    def kill_process(self, pid):
        """ Kill a scripted process.

        Args:
            pid (int): Process ID for the process to be killed.

        Returns:
            lldb.SBError: A status object notifying if the shutdown succeeded.
        """
        pass
