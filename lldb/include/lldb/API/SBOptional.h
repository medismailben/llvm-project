//===-- SBOptional.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_API_SBOPTIONAL_H
#define LLDB_API_SBOPTIONAL_H

#include "lldb/API/LLDB.h"

#include "lldb/API/SBAddress.h"
#include "lldb/API/SBAddressRange.h"
#include "lldb/API/SBAddressRangeList.h"
#include "lldb/API/SBAttachInfo.h"
#include "lldb/API/SBBlock.h"
#include "lldb/API/SBBreakpoint.h"
#include "lldb/API/SBBreakpointLocation.h"
#include "lldb/API/SBBreakpointName.h"
#include "lldb/API/SBBroadcaster.h"
#include "lldb/API/SBCommandInterpreter.h"
#include "lldb/API/SBCommandInterpreterRunOptions.h"
#include "lldb/API/SBCommandReturnObject.h"
#include "lldb/API/SBCommunication.h"
#include "lldb/API/SBCompileUnit.h"
#include "lldb/API/SBData.h"
#include "lldb/API/SBDebugger.h"
#include "lldb/API/SBDeclaration.h"
#include "lldb/API/SBEnvironment.h"
#include "lldb/API/SBError.h"
#include "lldb/API/SBEvent.h"
#include "lldb/API/SBExecutionContext.h"
#include "lldb/API/SBExpressionOptions.h"
#include "lldb/API/SBFile.h"
#include "lldb/API/SBFileSpec.h"
#include "lldb/API/SBFileSpecList.h"
#include "lldb/API/SBFormat.h"
#include "lldb/API/SBFrame.h"
#include "lldb/API/SBFunction.h"
#include "lldb/API/SBHostOS.h"
#include "lldb/API/SBInstruction.h"
#include "lldb/API/SBInstructionList.h"
#include "lldb/API/SBLanguageRuntime.h"
#include "lldb/API/SBLaunchInfo.h"
#include "lldb/API/SBLineEntry.h"
#include "lldb/API/SBListener.h"
#include "lldb/API/SBMemoryRegionInfo.h"
#include "lldb/API/SBMemoryRegionInfoList.h"
#include "lldb/API/SBModule.h"
#include "lldb/API/SBModuleSpec.h"
#include "lldb/API/SBMutex.h"
#include "lldb/API/SBPlatform.h"
#include "lldb/API/SBProcess.h"
#include "lldb/API/SBProcessInfo.h"
#include "lldb/API/SBProcessInfoList.h"
#include "lldb/API/SBProgress.h"
#include "lldb/API/SBQueue.h"
#include "lldb/API/SBQueueItem.h"
#include "lldb/API/SBReproducer.h"
#include "lldb/API/SBSaveCoreOptions.h"
#include "lldb/API/SBScriptObject.h"
#include "lldb/API/SBSection.h"
#include "lldb/API/SBSourceManager.h"
#include "lldb/API/SBStatisticsOptions.h"
#include "lldb/API/SBStream.h"
#include "lldb/API/SBStringList.h"
#include "lldb/API/SBStructuredData.h"
#include "lldb/API/SBSymbol.h"
#include "lldb/API/SBSymbolContext.h"
#include "lldb/API/SBSymbolContextList.h"
#include "lldb/API/SBTarget.h"
#include "lldb/API/SBThread.h"
#include "lldb/API/SBThreadCollection.h"
#include "lldb/API/SBThreadPlan.h"
#include "lldb/API/SBTrace.h"
#include "lldb/API/SBTraceCursor.h"
#include "lldb/API/SBType.h"
#include "lldb/API/SBTypeCategory.h"
#include "lldb/API/SBTypeEnumMember.h"
#include "lldb/API/SBTypeFilter.h"
#include "lldb/API/SBTypeFormat.h"
#include "lldb/API/SBTypeNameSpecifier.h"
#include "lldb/API/SBTypeSummary.h"
#include "lldb/API/SBTypeSynthetic.h"
#include "lldb/API/SBUnixSignals.h"
#include "lldb/API/SBValue.h"
#include "lldb/API/SBValueList.h"
#include "lldb/API/SBVariablesOptions.h"
#include "lldb/API/SBWatchpoint.h"
#include "lldb/API/SBWatchpointOptions.h"

#include <any>

namespace lldb {

class LLDB_API SBOptional {
public:
  enum Kind {
    Invalid,
    SBAddress,
    SBAddressRange,
    SBAddressRangeList,
    SBAttachInfo,
    SBBlock,
    SBBreakpoint,
    //    SBBreakpointList,
    SBBreakpointLocation,
    SBBreakpointName,
    SBBroadcaster,
    SBCommand,
    SBCommandInterpreter,
    SBCommandInterpreterRunOptions,
    SBCommandInterpreterRunResult,
    SBCommandPluginInterface,
    SBCommandReturnObject,
    //    SBCommunication,
    SBCompileUnit,
    SBData,
    SBDebugger,
    SBDeclaration,
    SBEnvironment,
    SBError,
    SBEvent,
    SBExecutionContext,
    SBExpressionOptions,
    SBFile,
    SBFileSpec,
    SBFileSpecList,
    SBFormat,
    SBFrame,
    SBFunction,
    SBHostOS,
    SBInputReader,
    SBInstruction,
    SBInstructionList,
    SBLanguageRuntime,
    //    SBLaunchInfo,
    SBLineEntry,
    SBListener,
    SBMemoryRegionInfo,
    SBMemoryRegionInfoList,
    SBModule,
    SBModuleSpec,
    SBModuleSpecList,
    SBMutex,
    SBPlatform,
    //    SBPlatformConnectOptions,
    //    SBPlatformShellCommand,
    SBProcess,
    SBProcessInfo,
    SBProcessInfoList,
    SBProgress,
    SBQueue,
    SBQueueItem,
    SBReplayOptions,
    SBReproducer,
    SBSaveCoreOptions,
    SBScriptObject,
    SBSection,
    SBSourceManager,
    SBStatisticsOptions,
    //    SBStream,
    SBStringList,
    SBStructuredData,
    SBSymbol,
    SBSymbolContext,
    SBSymbolContextList,
    SBTarget,
    SBThread,
    SBThreadCollection,
    SBThreadPlan,
    SBTrace,
    SBTraceCursor,
    SBType,
    SBTypeCategory,
    SBTypeEnumMember,
    SBTypeEnumMemberList,
    SBTypeFilter,
    SBTypeFormat,
    SBTypeList,
    SBTypeMember,
    SBTypeMemberFunction,
    SBTypeNameSpecifier,
    SBTypeStaticField,
    SBTypeSummary,
    //    SBTypeSummaryOptions,
    SBTypeSynthetic,
    SBUnixSignals,
    SBValue,
    SBValueList,
    SBVariablesOptions,
    SBWatchpoint,
    SBWatchpointOptions
  };

  //  struct Invalid {};

  SBOptional() : m_kind(Kind::Invalid), m_storage() {}

  SBOptional(lldb::SBAddress value)
      : m_kind(Kind::SBAddress), m_storage(value) {}
  SBOptional(lldb::SBAddressRange value)
      : m_kind(Kind::SBAddressRange), m_storage(value) {}
  SBOptional(lldb::SBAddressRangeList value)
      : m_kind(Kind::SBAddressRangeList), m_storage(value) {}
  SBOptional(lldb::SBAttachInfo value)
      : m_kind(Kind::SBAttachInfo), m_storage(value) {}
  SBOptional(lldb::SBBlock value) : m_kind(Kind::SBBlock), m_storage(value) {}
  SBOptional(lldb::SBBreakpoint value)
      : m_kind(Kind::SBBreakpoint), m_storage(value) {}
  //  SBOptional(lldb::SBBreakpointList value) : m_kind(Kind::SBBreakpointList),
  //  m_storage(value) {}
  SBOptional(lldb::SBBreakpointLocation value)
      : m_kind(Kind::SBBreakpointLocation), m_storage(value) {}
  SBOptional(lldb::SBBreakpointName value)
      : m_kind(Kind::SBBreakpointName), m_storage(value) {}
  SBOptional(lldb::SBBroadcaster value)
      : m_kind(Kind::SBBroadcaster), m_storage(value) {}
  SBOptional(lldb::SBCommand value)
      : m_kind(Kind::SBCommand), m_storage(value) {}
  SBOptional(lldb::SBCommandInterpreter value)
      : m_kind(Kind::SBCommandInterpreter), m_storage(value) {}
  SBOptional(lldb::SBCommandInterpreterRunOptions value)
      : m_kind(Kind::SBCommandInterpreterRunOptions), m_storage(value) {}
  SBOptional(lldb::SBCommandInterpreterRunResult value)
      : m_kind(Kind::SBCommandInterpreterRunResult), m_storage(value) {}
  SBOptional(lldb::SBCommandPluginInterface value)
      : m_kind(Kind::SBCommandPluginInterface), m_storage(value) {}
  SBOptional(lldb::SBCommandReturnObject value)
      : m_kind(Kind::SBCommandReturnObject), m_storage(value) {}
  //  SBOptional(lldb::SBCommunication value) : m_kind(Kind::SBCommunication),
  //  m_storage(value) {}
  SBOptional(lldb::SBCompileUnit value)
      : m_kind(Kind::SBCompileUnit), m_storage(value) {}
  SBOptional(lldb::SBData value) : m_kind(Kind::SBData), m_storage(value) {}
  SBOptional(lldb::SBDebugger value)
      : m_kind(Kind::SBDebugger), m_storage(value) {}
  SBOptional(lldb::SBDeclaration value)
      : m_kind(Kind::SBDeclaration), m_storage(value) {}
  SBOptional(lldb::SBEnvironment value)
      : m_kind(Kind::SBEnvironment), m_storage(value) {}
  SBOptional(lldb::SBError value) : m_kind(Kind::SBError), m_storage(value) {}
  SBOptional(lldb::SBEvent value) : m_kind(Kind::SBEvent), m_storage(value) {}
  SBOptional(lldb::SBExecutionContext value)
      : m_kind(Kind::SBExecutionContext), m_storage(value) {}
  SBOptional(lldb::SBExpressionOptions value)
      : m_kind(Kind::SBExpressionOptions), m_storage(value) {}
  SBOptional(lldb::SBFile value) : m_kind(Kind::SBFile), m_storage(value) {}
  SBOptional(lldb::SBFileSpec value)
      : m_kind(Kind::SBFileSpec), m_storage(value) {}
  SBOptional(lldb::SBFileSpecList value)
      : m_kind(Kind::SBFileSpecList), m_storage(value) {}
  SBOptional(lldb::SBFormat value) : m_kind(Kind::SBFormat), m_storage(value) {}
  SBOptional(lldb::SBFrame value) : m_kind(Kind::SBFrame), m_storage(value) {}
  SBOptional(lldb::SBFunction value)
      : m_kind(Kind::SBFunction), m_storage(value) {}
  SBOptional(lldb::SBHostOS value) : m_kind(Kind::SBHostOS), m_storage(value) {}
  SBOptional(lldb::SBInputReader value)
      : m_kind(Kind::SBInputReader), m_storage(value) {}
  SBOptional(lldb::SBInstruction value)
      : m_kind(Kind::SBInstruction), m_storage(value) {}
  SBOptional(lldb::SBInstructionList value)
      : m_kind(Kind::SBInstructionList), m_storage(value) {}
  SBOptional(lldb::SBLanguageRuntime value)
      : m_kind(Kind::SBLanguageRuntime), m_storage(value) {}
  //  SBOptional(lldb::SBLaunchInfo value) : m_kind(Kind::SBLaunchInfo),
  //  m_storage(value) {}
  SBOptional(lldb::SBLineEntry value)
      : m_kind(Kind::SBLineEntry), m_storage(value) {}
  SBOptional(lldb::SBListener value)
      : m_kind(Kind::SBListener), m_storage(value) {}
  SBOptional(lldb::SBMemoryRegionInfo value)
      : m_kind(Kind::SBMemoryRegionInfo), m_storage(value) {}
  SBOptional(lldb::SBMemoryRegionInfoList value)
      : m_kind(Kind::SBMemoryRegionInfoList), m_storage(value) {}
  SBOptional(lldb::SBModule value) : m_kind(Kind::SBModule), m_storage(value) {}
  SBOptional(lldb::SBModuleSpec value)
      : m_kind(Kind::SBModuleSpec), m_storage(value) {}
  SBOptional(lldb::SBModuleSpecList value)
      : m_kind(Kind::SBModuleSpecList), m_storage(value) {}
  SBOptional(lldb::SBMutex value) : m_kind(Kind::SBMutex), m_storage(value) {}
  SBOptional(lldb::SBPlatform value)
      : m_kind(Kind::SBPlatform), m_storage(value) {}
  //  SBOptional(lldb::SBPlatformConnectOptions value) :
  //  m_kind(Kind::SBPlatformConnectOptions), m_storage(value) {}
  //  SBOptional(lldb::SBPlatformShellCommand value) :
  //  m_kind(Kind::SBPlatformShellCommand), m_storage(value) {}
  SBOptional(lldb::SBProcess value)
      : m_kind(Kind::SBProcess), m_storage(value) {}
  SBOptional(lldb::SBProcessInfo value)
      : m_kind(Kind::SBProcessInfo), m_storage(value) {}
  SBOptional(lldb::SBProcessInfoList value)
      : m_kind(Kind::SBProcessInfoList), m_storage(value) {}
  //  SBOptional(lldb::SBProgress value) : m_kind(Kind::SBProgress),
  //  m_storage(value) {}
  SBOptional(lldb::SBQueue value) : m_kind(Kind::SBQueue), m_storage(value) {}
  SBOptional(lldb::SBQueueItem value)
      : m_kind(Kind::SBQueueItem), m_storage(value) {}
  SBOptional(lldb::SBReplayOptions value)
      : m_kind(Kind::SBReplayOptions), m_storage(value) {}
  SBOptional(lldb::SBReproducer value)
      : m_kind(Kind::SBReproducer), m_storage(value) {}
  SBOptional(lldb::SBSaveCoreOptions value)
      : m_kind(Kind::SBSaveCoreOptions), m_storage(value) {}
  //  SBOptional(lldb::SBScriptObject value) : m_kind(Kind::SBScriptObject),
  //  m_storage(value) {}
  SBOptional(lldb::SBSection value)
      : m_kind(Kind::SBSection), m_storage(value) {}
  //  SBOptional(lldb::SBSourceManager value) : m_kind(Kind::SBSourceManager),
  //  m_storage(value) {}
  SBOptional(lldb::SBStatisticsOptions value)
      : m_kind(Kind::SBStatisticsOptions), m_storage(value) {}
  //  SBOptional(lldb::SBStream value) : m_kind(Kind::SBStream),
  //  m_storage(value) {}
  SBOptional(lldb::SBStringList value)
      : m_kind(Kind::SBStringList), m_storage(value) {}
  SBOptional(lldb::SBStructuredData value)
      : m_kind(Kind::SBStructuredData), m_storage(value) {}
  SBOptional(lldb::SBSymbol value) : m_kind(Kind::SBSymbol), m_storage(value) {}
  SBOptional(lldb::SBSymbolContext value)
      : m_kind(Kind::SBSymbolContext), m_storage(value) {}
  SBOptional(lldb::SBSymbolContextList value)
      : m_kind(Kind::SBSymbolContextList), m_storage(value) {}
  SBOptional(lldb::SBTarget value) : m_kind(Kind::SBTarget), m_storage(value) {}
  SBOptional(lldb::SBThread value) : m_kind(Kind::SBThread), m_storage(value) {}
  SBOptional(lldb::SBThreadCollection value)
      : m_kind(Kind::SBThreadCollection), m_storage(value) {}
  SBOptional(lldb::SBThreadPlan value)
      : m_kind(Kind::SBThreadPlan), m_storage(value) {}
  SBOptional(lldb::SBTrace value) : m_kind(Kind::SBTrace), m_storage(value) {}
  SBOptional(lldb::SBTraceCursor value)
      : m_kind(Kind::SBTraceCursor), m_storage(value) {}
  SBOptional(lldb::SBType value) : m_kind(Kind::SBType), m_storage(value) {}
  SBOptional(lldb::SBTypeCategory value)
      : m_kind(Kind::SBTypeCategory), m_storage(value) {}
  SBOptional(lldb::SBTypeEnumMember value)
      : m_kind(Kind::SBTypeEnumMember), m_storage(value) {}
  SBOptional(lldb::SBTypeEnumMemberList value)
      : m_kind(Kind::SBTypeEnumMemberList), m_storage(value) {}
  SBOptional(lldb::SBTypeFilter value)
      : m_kind(Kind::SBTypeFilter), m_storage(value) {}
  SBOptional(lldb::SBTypeFormat value)
      : m_kind(Kind::SBTypeFormat), m_storage(value) {}
  SBOptional(lldb::SBTypeList value)
      : m_kind(Kind::SBTypeList), m_storage(value) {}
  SBOptional(lldb::SBTypeMember value)
      : m_kind(Kind::SBTypeMember), m_storage(value) {}
  SBOptional(lldb::SBTypeMemberFunction value)
      : m_kind(Kind::SBTypeMemberFunction), m_storage(value) {}
  SBOptional(lldb::SBTypeNameSpecifier value)
      : m_kind(Kind::SBTypeNameSpecifier), m_storage(value) {}
  SBOptional(lldb::SBTypeStaticField value)
      : m_kind(Kind::SBTypeStaticField), m_storage(value) {}
  SBOptional(lldb::SBTypeSummary value)
      : m_kind(Kind::SBTypeSummary), m_storage(value) {}
  //  SBOptional(lldb::SBTypeSummaryOptions value) :
  //  m_kind(Kind::SBTypeSummaryOptions), m_storage(value) {}
  SBOptional(lldb::SBTypeSynthetic value)
      : m_kind(Kind::SBTypeSynthetic), m_storage(value) {}
  SBOptional(lldb::SBUnixSignals value)
      : m_kind(Kind::SBUnixSignals), m_storage(value) {}
  SBOptional(lldb::SBValue value) : m_kind(Kind::SBValue), m_storage(value) {}
  SBOptional(lldb::SBValueList value)
      : m_kind(Kind::SBValueList), m_storage(value) {}
  SBOptional(lldb::SBVariablesOptions value)
      : m_kind(Kind::SBVariablesOptions), m_storage(value) {}
  SBOptional(lldb::SBWatchpoint value)
      : m_kind(Kind::SBWatchpoint), m_storage(value) {}
  SBOptional(lldb::SBWatchpointOptions value)
      : m_kind(Kind::SBWatchpointOptions), m_storage(value) {}

  ~SBOptional() = default;

  explicit operator bool() const { return HasValue(); }

  // operator== is a free function

  //  bool operator!=(const SBOptional &rhs) const;

  bool HasValue() const { return m_kind != Kind::Invalid; }

  Kind GetKind() const { return m_kind; }

  void Clear() { m_kind = Invalid; m_storage.reset(); }

  lldb::SBAddress GetSBAddress() const {
    if (const lldb::SBAddress *value =
            std::any_cast<lldb::SBAddress>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBAddressRange GetSBAddressRange() const {
    if (const lldb::SBAddressRange *value =
            std::any_cast<lldb::SBAddressRange>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBAddressRangeList GetSBAddressRangeList() const {
    if (const lldb::SBAddressRangeList *value =
            std::any_cast<lldb::SBAddressRangeList>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBAttachInfo GetSBAttachInfo() const {
    if (const lldb::SBAttachInfo *value =
            std::any_cast<lldb::SBAttachInfo>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBBlock GetSBBlock() const {
    if (const lldb::SBBlock *value = std::any_cast<lldb::SBBlock>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBBreakpoint GetSBBreakpoint() const {
    if (const lldb::SBBreakpoint *value =
            std::any_cast<lldb::SBBreakpoint>(&m_storage))
      return *value;
    return {};
  }

  //  lldb::SBBreakpointList GetSBBreakpointList() const {
  //   if (const lldb::SBBreakpointList* value =
  //   std::any_cast<lldb::SBBreakpointList>(&m_storage))
  //     return *value;
  //   return {};
  //  }

  lldb::SBBreakpointLocation GetSBBreakpointLocation() const {
    if (const lldb::SBBreakpointLocation *value =
            std::any_cast<lldb::SBBreakpointLocation>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBBreakpointName GetSBBreakpointName() const {
    if (const lldb::SBBreakpointName *value =
            std::any_cast<lldb::SBBreakpointName>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBBroadcaster GetSBBroadcaster() const {
    if (const lldb::SBBroadcaster *value =
            std::any_cast<lldb::SBBroadcaster>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBCommand GetSBCommand() const {
    if (const lldb::SBCommand *value =
            std::any_cast<lldb::SBCommand>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBCommandInterpreter GetSBCommandInterpreter() const {
    if (const lldb::SBCommandInterpreter *value =
            std::any_cast<lldb::SBCommandInterpreter>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBCommandInterpreterRunOptions
  GetSBCommandInterpreterRunOptions() const {
    if (const lldb::SBCommandInterpreterRunOptions *value =
            std::any_cast<lldb::SBCommandInterpreterRunOptions>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBCommandInterpreterRunResult GetSBCommandInterpreterRunResult() const {
    if (const lldb::SBCommandInterpreterRunResult *value =
            std::any_cast<lldb::SBCommandInterpreterRunResult>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBCommandPluginInterface GetSBCommandPluginInterface() const {
    if (const lldb::SBCommandPluginInterface *value =
            std::any_cast<lldb::SBCommandPluginInterface>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBCommandReturnObject GetSBCommandReturnObject() const {
    if (const lldb::SBCommandReturnObject *value =
            std::any_cast<lldb::SBCommandReturnObject>(&m_storage))
      return *value;
    return {};
  }

  //  lldb::SBCommunication GetSBCommunication() const {
  //   if (const lldb::SBCommunication* value =
  //   std::any_cast<lldb::SBCommunication>(&m_storage))
  // *value;
  //   return {};
  //  }

  lldb::SBCompileUnit GetSBCompileUnit() const {
    if (const lldb::SBCompileUnit *value =
            std::any_cast<lldb::SBCompileUnit>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBData GetSBData() const {
    if (const lldb::SBData *value = std::any_cast<lldb::SBData>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBDebugger GetSBDebugger() const {
    if (const lldb::SBDebugger *value =
            std::any_cast<lldb::SBDebugger>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBDeclaration GetSBDeclaration() const {
    if (const lldb::SBDeclaration *value =
            std::any_cast<lldb::SBDeclaration>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBEnvironment GetSBEnvironment() const {
    if (const lldb::SBEnvironment *value =
            std::any_cast<lldb::SBEnvironment>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBError GetSBError() const {
    if (const lldb::SBError *value = std::any_cast<lldb::SBError>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBEvent GetSBEvent() const {
    if (const lldb::SBEvent *value = std::any_cast<lldb::SBEvent>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBExecutionContext GetSBExecutionContext() const {
    if (const lldb::SBExecutionContext *value =
            std::any_cast<lldb::SBExecutionContext>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBExpressionOptions GetSBExpressionOptions() const {
    if (const lldb::SBExpressionOptions *value =
            std::any_cast<lldb::SBExpressionOptions>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBFile GetSBFile() const {
    if (const lldb::SBFile *value = std::any_cast<lldb::SBFile>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBFileSpec GetSBFileSpec() const {
    if (const lldb::SBFileSpec *value =
            std::any_cast<lldb::SBFileSpec>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBFileSpecList GetSBFileSpecList() const {
    if (const lldb::SBFileSpecList *value =
            std::any_cast<lldb::SBFileSpecList>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBFormat GetSBFormat() const {
    if (const lldb::SBFormat *value = std::any_cast<lldb::SBFormat>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBFrame GetSBFrame() const {
    if (const lldb::SBFrame *value = std::any_cast<lldb::SBFrame>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBFunction GetSBFunction() const {
    if (const lldb::SBFunction *value =
            std::any_cast<lldb::SBFunction>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBHostOS GetSBHostOS() const {
    if (const lldb::SBHostOS *value = std::any_cast<lldb::SBHostOS>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBInputReader GetSBInputReader() const {
    if (const lldb::SBInputReader *value =
            std::any_cast<lldb::SBInputReader>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBInstruction GetSBInstruction() const {
    if (const lldb::SBInstruction *value =
            std::any_cast<lldb::SBInstruction>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBInstructionList GetSBInstructionList() const {
    if (const lldb::SBInstructionList *value =
            std::any_cast<lldb::SBInstructionList>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBLanguageRuntime GetSBLanguageRuntime() const {
    if (const lldb::SBLanguageRuntime *value =
            std::any_cast<lldb::SBLanguageRuntime>(&m_storage))
      return *value;
    return {};
  }

  //  lldb::SBLaunchInfo GetSBLaunchInfo() const {
  //   if (const lldb::SBLaunchInfo* value =
  //   std::any_cast<lldb::SBLaunchInfo>(&m_storage))
  // *value;
  //   return {};
  //  }

  lldb::SBLineEntry GetSBLineEntry() const {
    if (const lldb::SBLineEntry *value =
            std::any_cast<lldb::SBLineEntry>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBListener GetSBListener() const {
    if (const lldb::SBListener *value =
            std::any_cast<lldb::SBListener>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBMemoryRegionInfo GetSBMemoryRegionInfo() const {
    if (const lldb::SBMemoryRegionInfo *value =
            std::any_cast<lldb::SBMemoryRegionInfo>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBMemoryRegionInfoList GetSBMemoryRegionInfoList() const {
    if (const lldb::SBMemoryRegionInfoList *value =
            std::any_cast<lldb::SBMemoryRegionInfoList>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBModule GetSBModule() const {
    if (const lldb::SBModule *value = std::any_cast<lldb::SBModule>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBModuleSpec GetSBModuleSpec() const {
    if (const lldb::SBModuleSpec *value =
            std::any_cast<lldb::SBModuleSpec>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBModuleSpecList GetSBModuleSpecList() const {
    if (const lldb::SBModuleSpecList *value =
            std::any_cast<lldb::SBModuleSpecList>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBMutex GetSBMutex() const {
    if (const lldb::SBMutex *value = std::any_cast<lldb::SBMutex>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBPlatform GetSBPlatform() const {
    if (const lldb::SBPlatform *value =
            std::any_cast<lldb::SBPlatform>(&m_storage))
      return *value;
    return {};
  }

  //  lldb::SBPlatformConnectOptions GetSBPlatformConnectOptions() const {
  //   if (const lldb::SBPlatformConnectOptions* value =
  //   std::any_cast<lldb::SBPlatformConnectOptions>(&m_storage))
  // *value;
  //   return {};
  //  }
  //
  //  lldb::SBPlatformShellCommand GetSBPlatformShellCommand() const {
  //   if (const lldb::SBPlatformShellCommand* value =
  //   std::any_cast<lldb::SBPlatformShellCommand>(&m_storage))
  // *value;
  //   return {};
  //  }

  lldb::SBProcess GetSBProcess() const {
    if (const lldb::SBProcess *value =
            std::any_cast<lldb::SBProcess>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBProcessInfo GetSBProcessInfo() const {
    if (const lldb::SBProcessInfo *value =
            std::any_cast<lldb::SBProcessInfo>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBProcessInfoList GetSBProcessInfoList() const {
    if (const lldb::SBProcessInfoList *value =
            std::any_cast<lldb::SBProcessInfoList>(&m_storage))
      return *value;
    return {};
  }

  //  lldb::SBProgress GetSBProgress() const {
  //   if (const lldb::SBProgress* value =
  //   std::any_cast<lldb::SBProgress>(&m_storage))
  // *value;
  //   return {};
  //  }

  lldb::SBQueue GetSBQueue() const {
    if (const lldb::SBQueue *value = std::any_cast<lldb::SBQueue>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBQueueItem GetSBQueueItem() const {
    if (const lldb::SBQueueItem *value =
            std::any_cast<lldb::SBQueueItem>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBReplayOptions GetSBReplayOptions() const {
    if (const lldb::SBReplayOptions *value =
            std::any_cast<lldb::SBReplayOptions>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBReproducer GetSBReproducer() const {
    if (const lldb::SBReproducer *value =
            std::any_cast<lldb::SBReproducer>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBSaveCoreOptions GetSBSaveCoreOptions() const {
    if (const lldb::SBSaveCoreOptions *value =
            std::any_cast<lldb::SBSaveCoreOptions>(&m_storage))
      return *value;
    return {};
  }

  //  lldb::SBScriptObject GetSBScriptObject() const {
  //   if (const lldb::SBScriptObject* value =
  //   std::any_cast<lldb::SBScriptObject>(&m_storage))
  // *value;
  //   return {};
  //  }

  lldb::SBSection GetSBSection() const {
    if (const lldb::SBSection *value =
            std::any_cast<lldb::SBSection>(&m_storage))
      return *value;
    return {};
  }

  //  lldb::SBSourceManager GetSBSourceManager() const {
  //   if (const lldb::SBSourceManager* value =
  //   std::any_cast<lldb::SBSourceManager>(&m_storage))
  // *value;
  //   return {};
  //  }

  lldb::SBStatisticsOptions GetSBStatisticsOptions() const {
    if (const lldb::SBStatisticsOptions *value =
            std::any_cast<lldb::SBStatisticsOptions>(&m_storage))
      return *value;
    return {};
  }

  //  lldb::SBStream GetSBStream() const {
  //   if (const lldb::SBStream* value =
  //   std::any_cast<lldb::SBStream>(&m_storage))
  // *value;
  //   return {};
  //  }

  lldb::SBStringList GetSBStringList() const {
    if (const lldb::SBStringList *value =
            std::any_cast<lldb::SBStringList>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBStructuredData GetSBStructuredData() const {
    if (const lldb::SBStructuredData *value =
            std::any_cast<lldb::SBStructuredData>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBSymbol GetSBSymbol() const {
    if (const lldb::SBSymbol *value = std::any_cast<lldb::SBSymbol>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBSymbolContext GetSBSymbolContext() const {
    if (const lldb::SBSymbolContext *value =
            std::any_cast<lldb::SBSymbolContext>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBSymbolContextList GetSBSymbolContextList() const {
    if (const lldb::SBSymbolContextList *value =
            std::any_cast<lldb::SBSymbolContextList>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTarget GetSBTarget() const {
    if (const lldb::SBTarget *value = std::any_cast<lldb::SBTarget>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBThread GetSBThread() const {
    if (const lldb::SBThread *value = std::any_cast<lldb::SBThread>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBThreadCollection GetSBThreadCollection() const {
    if (const lldb::SBThreadCollection *value =
            std::any_cast<lldb::SBThreadCollection>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBThreadPlan GetSBThreadPlan() const {
    if (const lldb::SBThreadPlan *value =
            std::any_cast<lldb::SBThreadPlan>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTrace GetSBTrace() const {
    if (const lldb::SBTrace *value = std::any_cast<lldb::SBTrace>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTraceCursor GetSBTraceCursor() const {
    if (const lldb::SBTraceCursor *value =
            std::any_cast<lldb::SBTraceCursor>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBType GetSBType() const {
    if (const lldb::SBType *value = std::any_cast<lldb::SBType>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTypeCategory GetSBTypeCategory() const {
    if (const lldb::SBTypeCategory *value =
            std::any_cast<lldb::SBTypeCategory>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTypeEnumMember GetSBTypeEnumMember() const {
    if (const lldb::SBTypeEnumMember *value =
            std::any_cast<lldb::SBTypeEnumMember>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTypeEnumMemberList GetSBTypeEnumMemberList() const {
    if (const lldb::SBTypeEnumMemberList *value =
            std::any_cast<lldb::SBTypeEnumMemberList>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTypeFilter GetSBTypeFilter() const {
    if (const lldb::SBTypeFilter *value =
            std::any_cast<lldb::SBTypeFilter>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTypeFormat GetSBTypeFormat() const {
    if (const lldb::SBTypeFormat *value =
            std::any_cast<lldb::SBTypeFormat>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTypeList GetSBTypeList() const {
    if (const lldb::SBTypeList *value =
            std::any_cast<lldb::SBTypeList>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTypeMember GetSBTypeMember() const {
    if (const lldb::SBTypeMember *value =
            std::any_cast<lldb::SBTypeMember>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTypeMemberFunction GetSBTypeMemberFunction() const {
    if (const lldb::SBTypeMemberFunction *value =
            std::any_cast<lldb::SBTypeMemberFunction>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTypeNameSpecifier GetSBTypeNameSpecifier() const {
    if (const lldb::SBTypeNameSpecifier *value =
            std::any_cast<lldb::SBTypeNameSpecifier>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTypeStaticField GetSBTypeStaticField() const {
    if (const lldb::SBTypeStaticField *value =
            std::any_cast<lldb::SBTypeStaticField>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBTypeSummary GetSBTypeSummary() const {
    if (const lldb::SBTypeSummary *value =
            std::any_cast<lldb::SBTypeSummary>(&m_storage))
      return *value;
    return {};
  }

  //  lldb::SBTypeSummaryOptions GetSBTypeSummaryOptions() const {
  //   if (const lldb::SBTypeSummaryOptions* value =
  //   std::any_cast<lldb::SBTypeSummaryOptions>(&m_storage))
  //     return *value;
  //   return {};
  //  }

  lldb::SBTypeSynthetic GetSBTypeSynthetic() const {
    if (const lldb::SBTypeSynthetic *value =
            std::any_cast<lldb::SBTypeSynthetic>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBUnixSignals GetSBUnixSignals() const {
    if (const lldb::SBUnixSignals *value =
            std::any_cast<lldb::SBUnixSignals>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBValue GetSBValue() const {
    if (const lldb::SBValue *value = std::any_cast<lldb::SBValue>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBValueList GetSBValueList() const {
    if (const lldb::SBValueList *value =
            std::any_cast<lldb::SBValueList>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBVariablesOptions GetSBVariablesOptions() const {
    if (const lldb::SBVariablesOptions *value =
            std::any_cast<lldb::SBVariablesOptions>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBWatchpoint GetSBWatchpoint() const {
    if (const lldb::SBWatchpoint *value =
            std::any_cast<lldb::SBWatchpoint>(&m_storage))
      return *value;
    return {};
  }

  lldb::SBWatchpointOptions GetSBWatchpointOptions() const {
    if (const lldb::SBWatchpointOptions *value =
            std::any_cast<lldb::SBWatchpointOptions>(&m_storage))
      return *value;
    return {};
  }

private:
  Kind m_kind;
  std::any m_storage;
};

} // namespace lldb

#endif // LLDB_API_SBOPTIONAL_H
