"""Standalone build-step helpers: compile a per-test binary directly with a
configured compiler, with no Makefile and no source-checkout dependency.

Unlike ``lldbsuite.test.lldbtest.TestBase.build()`` (which drives
``lldb/test/API/make/Makefile.rules``), this invokes the compiler directly.
It's sufficient for compiling a single source file (or a short explicit
list) with no per-arch/per-compiler build matrix; override :meth:`build`
in a subclass for a different build strategy.
"""

from __future__ import annotations

import inspect
import os
import subprocess
import tempfile

_SOURCE_EXTENSION_COMPILERS = {
    ".c": ("CC", "cc"),
    ".m": ("CC", "cc"),
    ".s": ("CC", "cc"),
    ".S": ("CC", "cc"),
    ".cpp": ("CXX", "c++"),
    ".cc": ("CXX", "c++"),
    ".mm": ("CXX", "c++"),
}

_DEFAULT_MAIN_NAMES = ("main.cpp", "main.cc", "main.c", "main.m", "main.mm")


class BuildMixin:
    """Mix into a ``unittest.TestCase`` (see
    :class:`lldb.testing.testcase.LLDBTestCase`, which already includes this)
    to compile per-test binaries via a direct compiler invocation.
    """

    def getSourceDir(self) -> str:
        """Return the directory containing this test's own source file."""
        return os.path.dirname(os.path.abspath(inspect.getfile(type(self))))

    def getBuildDir(self) -> str:
        """Return (creating if needed) a scratch directory for this test's
        build artifacts.

        If the ``LLDB_TESTING_BUILD_DIR`` environment variable is set (as
        the lit-integration runner does), the directory is created under
        it, named after the test class and method. Otherwise a fresh
        temporary directory is used.
        """
        build_dir = getattr(self, "_build_dir", None)
        if build_dir is None:
            root = os.environ.get("LLDB_TESTING_BUILD_DIR")
            if root:
                build_dir = os.path.join(root, type(self).__name__, self._testMethodName)
                os.makedirs(build_dir, exist_ok=True)
            else:
                build_dir = tempfile.mkdtemp(prefix="lldb-testing-")
            self._build_dir = build_dir
        return build_dir

    def getBuildArtifact(self, name: str = "a.out") -> str:
        """Return the path to a named build artifact under :meth:`getBuildDir`."""
        return os.path.join(self.getBuildDir(), name)

    def build(
        self,
        sources: list[str] | None = None,
        exe_name: str = "a.out",
        extra_flags: list[str] | None = None,
        dictionary: dict[str, str] | None = None,
    ) -> str:
        """Compile ``sources`` into :meth:`getBuildArtifact`.

        Args:
            sources: Source file paths, relative to :meth:`getSourceDir` if
                not absolute. Defaults to the first of ``main.cpp``/
                ``main.cc``/``main.c``/``main.m``/``main.mm`` found there.
            exe_name: The output binary's filename.
            extra_flags: Extra compiler flags, appended after the defaults.
            dictionary: Overrides for the compiler to use; ``dictionary["CC"]``/
                ``dictionary["CXX"]`` take precedence over the ``CC``/``CXX``
                environment variables and the built-in default.

        Returns:
            The path to the compiled binary.

        Raises:
            RuntimeError: If no source files were found, or compilation failed.
        """
        source_dir = self.getSourceDir()
        if sources is None:
            for candidate in _DEFAULT_MAIN_NAMES:
                if os.path.exists(os.path.join(source_dir, candidate)):
                    sources = [candidate]
                    break
            else:
                raise RuntimeError(f"No default main.* source found in {source_dir}")

        resolved_sources = [
            source if os.path.isabs(source) else os.path.join(source_dir, source)
            for source in sources
        ]
        _, ext = os.path.splitext(resolved_sources[0])
        env_var, default_compiler = _SOURCE_EXTENSION_COMPILERS.get(ext, ("CC", "cc"))
        dictionary = dictionary or {}
        compiler = dictionary.get(env_var) or os.environ.get(env_var, default_compiler)

        exe_path = self.getBuildArtifact(exe_name)
        command = [
            compiler,
            "-g",
            "-O0",
            *resolved_sources,
            "-o",
            exe_path,
            *(extra_flags or []),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(
                f"Build failed: {' '.join(command)}\n{result.stdout}\n{result.stderr}"
            )
        return exe_path
