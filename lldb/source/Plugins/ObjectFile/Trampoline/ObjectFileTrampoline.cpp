//===-- ObjectFileTrampoline.cpp ------------------------------ -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ObjectFileTrampoline.h"

#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Section.h"
#include "lldb/Target/SectionLoadList.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/DataBufferHeap.h"

using namespace lldb;
using namespace lldb_private;

LLDB_PLUGIN_DEFINE(ObjectFileTrampoline)

void ObjectFileTrampoline::Initialize() {
  PluginManager::RegisterPlugin(
      GetPluginNameStatic(), GetPluginDescriptionStatic(), CreateInstance,
      CreateMemoryInstance, GetModuleSpecifications);
}

void ObjectFileTrampoline::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

const char *ObjectFileTrampoline::GetPluginDescriptionStatic() {
  return "Jitted Conditional Breakpoint Trampoline code object file";
}

ObjectFile *ObjectFileTrampoline::CreateInstance(
    const lldb::ModuleSP &module_sp, lldb::DataExtractorSP extractor_sp,
    lldb::offset_t data_offset, const FileSpec *file,
    lldb::offset_t file_offset, lldb::offset_t length) {
  return nullptr;
}

ObjectFile *ObjectFileTrampoline::CreateMemoryInstance(
    const lldb::ModuleSP &module_sp, WritableDataBufferSP data_sp,
    const ProcessSP &process_sp, lldb::addr_t header_addr) {
  return nullptr;
}

ModuleSpecList ObjectFileTrampoline::GetModuleSpecifications(
    const lldb_private::FileSpec &file, lldb::DataExtractorSP &extractor_sp,
    lldb::offset_t file_offset, lldb::offset_t length) {
  // A trampoline only ever exists in the inferior's memory, never in a file.
  return {};
}

ObjectFileTrampoline::ObjectFileTrampoline(const lldb::ModuleSP &module_sp,
                                           lldb::ProcessSP process_sp,
                                           lldb::addr_t address,
                                           std::size_t size)
    : ObjectFile(module_sp, process_sp, address,
                 std::make_shared<DataExtractor>()),
      m_process_wp(process_sp), m_address(address), m_size(size) {
  const ArchSpec &arch = process_sp->GetTarget().GetArchitecture();
  m_data_nsp->SetByteOrder(arch.GetByteOrder());
  m_data_nsp->SetAddressByteSize(arch.GetAddressByteSize());
  m_file = lldb_private::FileSpec("$__lldb_jitted_conditional_bp_trampoline");
}

bool ObjectFileTrampoline::ParseHeader() {
  // JIT code is never in a file, nor is it required to have any header
  return false;
}

ByteOrder ObjectFileTrampoline::GetByteOrder() const {
  return m_data_nsp->GetByteOrder();
}

bool ObjectFileTrampoline::IsExecutable() const { return true; }

uint32_t ObjectFileTrampoline::GetAddressByteSize() const {
  return m_data_nsp->GetAddressByteSize();
}

void ObjectFileTrampoline::ParseSymtab(lldb_private::Symtab &symtab) {
  Log *log = GetLog(LLDBLog::JITLoader);

  uint32_t symID = 0;
  std::string name = "$__lldb_jitted_conditional_bp_trampoline";
  //      bool name_is_mangled = false;
  lldb::SymbolType type = eSymbolTypeCode;
  bool external = false;
  bool is_debug = false;
  bool is_trampoline = false;
  bool is_artificial = false;
  bool size_is_valid = true;
  bool contains_linker_annotions = false;
  uint32_t flags = 0; // TODO: Find flags
  const lldb::SectionSP &section_sp = m_sections_up->GetSectionAtIndex(0);

  if (!section_sp) {
    LLDB_LOG(log, "Couldn't find any section for Trampoline");
    return;
  }

  const Symbol symbol(symID, name.c_str(), type, external, is_debug,
                      is_trampoline, is_artificial, section_sp, 0, m_size,
                      size_is_valid, contains_linker_annotions, flags);

  m_symtab_up->AddSymbol(symbol);
  m_symtab_up->Finalize();
}

bool ObjectFileTrampoline::IsStripped() {
  return false; // JIT code that is in a module is never stripped
}

void ObjectFileTrampoline::CreateSections(SectionList &unified_section_list) {
  if (!m_sections_up) {
    m_sections_up.reset(new SectionList());

    // FIXME: Get real values
    user_id_t id = 0;
    std::string name = "$__lldb_jitted_conditional_bp_trampoline.__text";
    SectionType type = eSectionTypeCode;
    lldb::addr_t file_vm_addr = m_address;
    lldb::addr_t vm_size = m_size;
    lldb::offset_t file_offset = 0;
    lldb::offset_t file_size = m_size;
    uint32_t permissions =
        lldb::ePermissionsReadable | lldb::ePermissionsExecutable;

    lldb::SectionSP section_sp(new lldb_private::Section(
        GetModule(), this, id, ConstString(name), type, file_vm_addr, vm_size,
        file_offset, file_size, 0, permissions));

    m_sections_up->AddSection(section_sp);

    unified_section_list = *m_sections_up;
  }
}

void ObjectFileTrampoline::Dump(Stream *strm) {
  ModuleSP module_sp(GetModule());
  if (module_sp) {
    std::lock_guard<std::recursive_mutex> guard(module_sp->GetMutex());
    strm->Printf("%p: ", static_cast<void *>(this));
    strm->Indent();
    strm->PutCString("ObjectFileTrampoline");

    if (ArchSpec arch = GetArchitecture())
      *strm << ", arch = " << arch.GetArchitectureName();

    strm->EOL();

    SectionList *sections = GetSectionList();
    if (sections)
      sections->Dump(strm->AsRawOstream(), strm->GetIndentLevel() + 2, nullptr,
                     true, UINT32_MAX);

    if (m_symtab_up)
      m_symtab_up->Dump(strm, NULL, eSortOrderNone);
  }
}

UUID ObjectFileTrampoline::GetUUID() { return UUID(); }

uint32_t ObjectFileTrampoline::GetDependentModules(FileSpecList &files) {
  // JIT modules don't have dependencies, but they could
  // if external functions are called and we know where they are
  files.Clear();
  return 0;
}

lldb_private::Address ObjectFileTrampoline::GetEntryPointAddress() {
  return Address(m_address);
}

lldb_private::Address ObjectFileTrampoline::GetBaseAddress() {
  return Address(m_address);
}

ObjectFile::Type ObjectFileTrampoline::CalculateType() { return eTypeJIT; }

ObjectFile::Strata ObjectFileTrampoline::CalculateStrata() {
  return eStrataJIT;
}

ArchSpec ObjectFileTrampoline::GetArchitecture() {
  ProcessSP process_sp = m_process_wp.lock();
  return process_sp->GetTarget().GetArchitecture();
}

//------------------------------------------------------------------
// PluginInterface protocol
//------------------------------------------------------------------
llvm::StringRef ObjectFileTrampoline::GetPluginName() {
  return GetPluginNameStatic();
}

bool ObjectFileTrampoline::SetLoadAddress(Target &target, lldb::addr_t value,
                                          bool value_is_offset) {
  size_t num_loaded_sections = 0;
  SectionList *section_list = GetSectionList();
  if (section_list) {
    const size_t num_sections = section_list->GetSize();
    // "value" is an offset to apply to each top level segment
    for (size_t sect_idx = 0; sect_idx < num_sections; ++sect_idx) {
      // Iterate through the object file sections to find all of the sections
      // that size on disk (to avoid __PAGEZERO) and load them
      SectionSP section_sp(section_list->GetSectionAtIndex(sect_idx));
      if (section_sp && section_sp->GetFileSize() > 0 &&
          !section_sp->IsThreadSpecific()) {
        if (target.SetSectionLoadAddress(section_sp,
                                         section_sp->GetFileAddress() + value))
          ++num_loaded_sections;
      }
    }
  }
  return num_loaded_sections > 0;
}

size_t ObjectFileTrampoline::ReadSectionData(lldb_private::Section *section,
                                             lldb::offset_t section_offset,
                                             void *dst, size_t dst_len) {
  Log *log = GetLog(LLDBLog::JITLoader);

  lldb::offset_t file_size = section->GetFileSize();
  if (section_offset < file_size) {
    size_t src_len = file_size - section_offset;
    if (src_len > dst_len)
      src_len = dst_len;

    ProcessSP process_sp = m_process_wp.lock();

    Status error;
    size_t read_memory = process_sp->ReadMemory(m_address, dst, src_len, error);

    if (read_memory != src_len || error.Fail()) {
      LLDB_LOG(log, "JIT: Couldn't read trampoline module section");
      return 0;
    }

    return read_memory;
  }
  return 0;
}

size_t ObjectFileTrampoline::ReadSectionData(
    lldb_private::Section *section, lldb_private::DataExtractor &section_data) {
  if (section->GetFileSize()) {
    const void *src = (void *)(uintptr_t)section->GetFileOffset();

    DataBufferSP data_sp(
        new lldb_private::DataBufferHeap(src, section->GetFileSize()));
    if (data_sp) {
      section_data.SetData(data_sp, 0, data_sp->GetByteSize());
      section_data.SetByteOrder(GetByteOrder());
      section_data.SetAddressByteSize(GetAddressByteSize());
      return section_data.GetByteSize();
    }
  }
  section_data.Clear();
  return 0;
}
