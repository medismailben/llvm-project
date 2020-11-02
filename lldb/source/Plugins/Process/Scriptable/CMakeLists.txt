lldb_tablegen(ScriptableProcessOptions.inc -gen-lldb-option-defs
  SOURCE ScriptableProcessOptions.td
  TARGET LLDBPluginScriptableProcessOptionGen)

add_lldb_library(lldbPluginScriptableProcess PLUGIN
  ScriptableProcess.cpp

  LINK_LIBS
    lldbCore
    lldbTarget
    lldbUtility
    lldbPluginProcessUtility
  LINK_COMPONENTS
    BinaryFormat
    Object
    Support
  )

add_dependencies(lldbPluginScriptableProcess
  LLDBPluginScriptableProcessOptionGen
)
