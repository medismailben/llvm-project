EXE = PlaygroundStub
SWIFT_SOURCES = PlaygroundStub.swift
# The real playground stub has no debug info and is written in C.
# Suppress Swift back-deployment compatibility shims: the test intentionally
# uses an older deployment target to exercise LLDB target-override behaviour,
# but always runs on a modern device so the shims are not needed.  Without this
# flag the linker looks for iOS compat static libs that our custom Swift
# toolchain build doesn't include.
SWIFTFLAGS_EXTRAS = -gnone -runtime-compatibility-version none

# On remote Darwin (iOS/tvOS/watchOS) the device doesn't have AuxSources.framework
# pre-installed, so linking it statically causes a dyld crash at launch.
# On local macOS the static link is required for do_import_test() to work.
LD_EXTRAS := -Xlinker -rpath -Xlinker /usr/lib/swift
LD_EXTRAS += -L. -lPlaygroundsRuntime -F. -framework AuxSources

PlaygroundStub: libPlaygroundsRuntime.dylib Dylib.framework AuxSources.framework

include Makefile.rules

libPlaygroundsRuntime.dylib: PlaygroundsRuntime.swift
	"$(MAKE)" -f $(MAKEFILE_RULES) \
		DYLIB_SWIFT_SOURCES=PlaygroundsRuntime.swift \
		DYLIB_NAME=PlaygroundsRuntime \
		SWIFTFLAGS_EXTRAS="-runtime-compatibility-version none"

Dylib.framework: Dylib.swift
	"$(MAKE)" -f $(MAKEFILE_RULES) \
		FRAMEWORK=Dylib \
		DYLIB_SWIFT_SOURCES=Dylib.swift \
		DYLIB_NAME=Dylib \
		SWIFTFLAGS_EXTRAS="-Xcc -DNEW_OPTION_FROM_DYLIB=1 -runtime-compatibility-version none"

AuxSources.framework: AuxSources.swift
	"$(MAKE)" -f $(MAKEFILE_RULES) \
		FRAMEWORK=AuxSources \
		DYLIB_SWIFT_SOURCES=AuxSources.swift \
		DYLIB_NAME=AuxSources \
		SWIFTFLAGS_EXTRAS="-Xcc -DHAVE_AUXSOURCES=1 -runtime-compatibility-version none"
