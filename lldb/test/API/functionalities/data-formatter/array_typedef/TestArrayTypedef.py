import lldb
from lldb.testing import lldbutil
from lldb.testing.testcase import LLDBTestCase


class ArrayTypedefTestCase(LLDBTestCase):
    def test_array_typedef(self):
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "// break here", lldb.SBFileSpec("main.cpp", False)
        )
        self.expect("expr str", substrs=['"abcd"'])
