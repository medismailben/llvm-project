import lldb
from lldb.testing import lldbutil
from lldb.testing.testcase import LLDBTestCase


class TestCase(LLDBTestCase):
    def test_summary_string_with_bare_dollar_char(self):
        self.build()
        lldbutil.run_to_source_breakpoint(self, "break here", lldb.SBFileSpec("main.c"))
        self.runCmd("type summary add --summary-string '$ $CASH $' --no-value Dollars")
        self.expect("v cash", startstr="(Dollars) cash = $ $CASH $")

    def test_summary_string_with_bare_dollar_char_before_var(self):
        self.build()
        lldbutil.run_to_source_breakpoint(self, "break here", lldb.SBFileSpec("main.c"))
        self.runCmd("type summary add --summary-string '$${var}' --no-value Dollars")
        self.expect("v cash", startstr="(Dollars) cash = $99")
