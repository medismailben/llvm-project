.. title:: clang-tidy - readability-argument-label-comments

readability-argument-label-comments
====================================

Inserts argument label comments before literal arguments in function calls to
improve code readability.

This check identifies function calls where literal values (numbers, strings,
booleans, ``nullptr``) are passed as arguments and suggests adding comments
that label these literals with their parameter names.

Labeled arguments make it immediately clear what each literal value represents,
especially in functions with multiple parameters of the same type.

Examples
--------

.. code-block:: c++

  void configure(int timeout, const char* host, bool debug) {}

  // Before
  configure(5000, "localhost", true);

  // After (with fix applied)
  configure(/*timeout=*/5000, /*host=*/"localhost", /*debug=*/true);

More examples:

.. code-block:: c++

  void setDimensions(int width, int height) {}

  // Before
  setDimensions(1920, 1080);

  // After
  setDimensions(/*width=*/1920, /*height=*/1080);

The check only suggests labels for literal arguments (integers, floats, strings,
booleans, and ``nullptr``). Variable names and expressions are not labeled since
the variable name itself provides context:

.. code-block:: c++

  int width = 1920;
  int height = 1080;

  // No warnings - variable names provide context
  setDimensions(width, height);

Options
-------

.. option:: MinimumArguments

   The minimum number of arguments a function must have before the check
   suggests adding labels. Functions with fewer arguments are not checked.
   Default is ``2``.

   For example, with ``MinimumArguments: 3``:

   .. code-block:: c++

     void foo(int x) {}
     void bar(int x, int y) {}
     void baz(int x, int y, int z) {}

     foo(42);           // Not checked (1 argument)
     bar(10, 20);       // Not checked (2 arguments)
     baz(1, 2, 3);      // Checked and will suggest labels

Rationale
---------

Literal arguments in function calls can be unclear, especially when:

- Multiple arguments have the same type
- The function name doesn't make the parameter purpose obvious
- Boolean literals are used (``true``/``false`` without context)
- Magic numbers are passed directly

Adding label comments improves code readability by making the intent explicit
at the call site, similar to named arguments in languages like Python or Swift.

Limitations
-----------

- The check only works when the function declaration is available in the
  current translation unit
- Template functions and overloaded operators may not be handled correctly
  in all cases
- Already existing label comments are detected, but only if they follow the
  exact ``/*name=*/`` pattern
