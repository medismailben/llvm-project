# Python Testing Helpers

`lldb.testing` lets LLDB scripting clients — authors of data formatters,
`ScriptedProcess`/`ScriptedThread` implementations, and scripted frame
providers, as well as custom lldb commands — write their own `unittest`-based
test suites. Unlike LLDB's own test suite (`lldbsuite.test`), `lldb.testing`
has no dependency on a source checkout, a build step, or any global test-
runner state: it works from inside any running lldb session, including one
loaded from a `command script import`-ed module or an auto-loaded dSYM.

See {doc}`python_extensions` for how to write the scripting extensions this
package helps you test.

## Test case base classes

```{eval-rst}
.. automodule:: lldb.testing.testcase

.. automodsumm:: lldb.testing.testcase
    :classes-only:
    :toctree: python_api
```

```{eval-rst}
.. automodule:: lldb.testing.command

.. automodsumm:: lldb.testing.command
    :classes-only:
    :toctree: python_api
```

```{eval-rst}
.. automodule:: lldb.testing.gdbremote_case

.. automodsumm:: lldb.testing.gdbremote_case
    :classes-only:
    :toctree: python_api
```

```{eval-rst}
.. automodule:: lldb.testing.pexpect_case

.. automodsumm:: lldb.testing.pexpect_case
    :classes-only:
    :toctree: python_api
```

```{eval-rst}
.. automodule:: lldb.testing.session

.. automodsumm:: lldb.testing.session
    :classes-only:
    :toctree: python_api
```

```{eval-rst}
.. automodule:: lldb.testing.build

.. automodsumm:: lldb.testing.build
    :classes-only:
    :toctree: python_api
```

## Assertions and value checking

```{eval-rst}
.. automodule:: lldb.testing.asserts

.. automodsumm:: lldb.testing.asserts
    :classes-only:
    :toctree: python_api
```

```{eval-rst}
.. automodule:: lldb.testing.expect

.. automodsumm:: lldb.testing.expect
    :classes-only:
    :toctree: python_api
```

```{eval-rst}
.. automodule:: lldb.testing.value_check

.. automodsumm:: lldb.testing.value_check
    :classes-only:
    :toctree: python_api
```

## Breakpoints and running

```{eval-rst}
.. automodule:: lldb.testing.lldbutil
```

```{eval-rst}
.. automodule:: lldb.testing.run_to_breakpoint
```

## Running and discovering test suites

```{eval-rst}
.. automodule:: lldb.testing.runner

.. automodsumm:: lldb.testing.runner
    :classes-only:
    :toctree: python_api
```

```{eval-rst}
.. automodule:: lldb.testing.discovery

.. automodsumm:: lldb.testing.discovery
    :classes-only:
    :toctree: python_api
```
