import lldbsuite.test.lldbinline as lldbinline
from lldbsuite.test.decorators import *

lldbinline.MakeInlineTest(
    __file__,
    globals(),
    decorators=[
        swiftTest,
        skipUnlessDarwin,
        expectedFailureAll(bugnumber="rdar://60396797", swift_variant="dwarfimporter"),
    ],
)
