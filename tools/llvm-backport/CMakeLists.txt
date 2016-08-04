set(LLVM_LINK_COMPONENTS
  IRReader
  Core
  Support
  )

add_llvm_tool(llvm-backport
  BitcodeWriter.cpp
  ValueEnumerator.cpp
  llvm-backport.cpp
  )
