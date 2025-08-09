//===-- ObjectFileTrampoline.h -------------------------------- -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_PLUGINS_OBJECTFILE_TRAMPOLINE_OBJECTFILETRAMPOLINE_H
#define LLDB_PLUGINS_OBJECTFILE_TRAMPOLINE_OBJECTFILETRAMPOLINE_H

#include "lldb/Core/Address.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/ArchSpec.h"
#include "lldb/lldb-defines.h"

namespace lldb_private {

//----------------------------------------------------------------------
// This class needs to be hidden as eventually belongs in a plugin that
// will export the ObjectFile protocol
//----------------------------------------------------------------------
class ObjectFileTrampoline : public ObjectFile {
public:
  ObjectFileTrampoline(const lldb::ModuleSP &module_sp,
                       lldb::ProcessSP process_sp, lldb::addr_t address,
                       std::size_t size);

  ~ObjectFileTrampoline() = default;
  
  //------------------------------------------------------------------
  // Static Functions
  //------------------------------------------------------------------
  static void Initialize();
  static void Terminate();

  static llvm::StringRef GetPluginNameStatic() { return "trampoline"; };

  static const char *GetPluginDescriptionStatic();

  static lldb_private::ObjectFile *
  CreateInstance(const lldb::ModuleSP &module_sp, lldb::DataBufferSP data_sp,
                 lldb::offset_t data_offset, const lldb_private::FileSpec *file,
                 lldb::offset_t file_offset, lldb::offset_t length);

  static lldb_private::ObjectFile *CreateMemoryInstance(
      const lldb::ModuleSP &module_sp, lldb::WritableDataBufferSP data_sp,
      const lldb::ProcessSP &process_sp, lldb::addr_t header_addr);

  static size_t GetModuleSpecifications(const lldb_private::FileSpec &file,
                                        lldb::DataBufferSP &data_sp,
                                        lldb::offset_t data_offset,
                                        lldb::offset_t file_offset,
                                        lldb::offset_t length,
                                        lldb_private::ModuleSpecList &specs);

  //------------------------------------------------------------------
  // Member Functions
  //------------------------------------------------------------------
  bool ParseHeader() override;

  bool SetLoadAddress(lldb_private::Target &target, lldb::addr_t value,
                      bool value_is_offset) override;

  lldb::ByteOrder GetByteOrder() const override;

  bool IsExecutable() const override;

  uint32_t GetAddressByteSize() const override;

  void ParseSymtab(lldb_private::Symtab &symtab) override;

  bool IsStripped() override;

  void CreateSections(lldb_private::SectionList &unified_section_list) override;

  void Dump(lldb_private::Stream *s) override;

  lldb_private::ArchSpec GetArchitecture() override;

  lldb_private::UUID GetUUID() override;

  uint32_t GetDependentModules(lldb_private::FileSpecList &files) override;

  size_t ReadSectionData(lldb_private::Section *section,
                         lldb::offset_t section_offset, void *dst,
                         size_t dst_len) override;

  size_t ReadSectionData(lldb_private::Section *section,
                         lldb_private::DataExtractor &section_data) override;

  lldb_private::Address GetEntryPointAddress() override;

  lldb_private::Address GetBaseAddress() override;

  ObjectFile::Type CalculateType() override;

  ObjectFile::Strata CalculateStrata() override;

  FileSpec &GetFileSpec() override { return m_file; }

  //------------------------------------------------------------------
  // PluginInterface protocol
  //------------------------------------------------------------------
  llvm::StringRef GetPluginName() override;

  //    /// LLVM RTTI support
  //    /// \{
  //  static char ID;
  //  bool isA(const void *ClassID) const override {
  //    return ClassID == &ID || ObjectFile::isA(ClassID);
  //  }
  //  static bool classof(const ObjectFile *obj) { return obj->isA(&ID); }
  //    /// \}

protected:
  lldb::ProcessWP m_process_wp;
  lldb::addr_t m_address;
  std::size_t m_size;
};

} // namespace lldb_private
#endif // LLDB_PLUGINS_OBJECTFILE_TRAMPOLINE_OBJECTFILETRAMPOLINE_H
