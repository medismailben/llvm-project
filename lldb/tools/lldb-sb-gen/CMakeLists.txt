if(APPLE)
  # add_compile_options(-isysroot ${SYSROOT})
endif()


add_lldb_tool(lldb-sb-gen
  SBGenerator.cpp

  LINK_COMPONENTS
    Support
  CLANG_LIBS
    clangAST
    clangBasic
    clangCodeGen
    clangFrontend
    clangLex
    clangRewrite
    clangSerialization
    clangTooling
  )
