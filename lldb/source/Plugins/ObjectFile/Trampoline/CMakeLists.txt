add_lldb_library(lldbPluginObjectFileTrampoline PLUGIN
  ObjectFileTrampoline.cpp

  LINK_LIBS
    lldbCore
    lldbHost
    lldbSymbol
    lldbUtility
  LINK_COMPONENTS
    Support
  )
