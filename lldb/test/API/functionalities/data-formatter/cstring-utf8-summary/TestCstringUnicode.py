# coding=utf8

import lldb
from lldb.testing import lldbutil
from lldb.testing.testcase import LLDBTestCase


class CstringUnicodeTestCase(LLDBTestCase):
    def test_cstring_unicode(self):
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "// break here", lldb.SBFileSpec("main.cpp", False)
        )
        self.expect_expr("s", result_summary='"🔥"')
        self.expect_expr("(const char*)s", result_summary='"🔥"')
