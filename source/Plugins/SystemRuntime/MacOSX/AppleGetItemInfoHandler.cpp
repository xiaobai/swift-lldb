//===-- AppleGetItemInfoHandler.cpp -------------------------------*- C++
//-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "AppleGetItemInfoHandler.h"

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes

#include "lldb/Core/Module.h"
#include "lldb/Core/Value.h"
#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Expression/FunctionCaller.h"
#include "lldb/Expression/UtilityFunction.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/StreamString.h"

using namespace lldb;
using namespace lldb_private;

const char *AppleGetItemInfoHandler::g_get_item_info_function_name =
    "__lldb_backtrace_recording_get_item_info";
const char *AppleGetItemInfoHandler::g_get_item_info_function_code =
    "                                  \n\
extern \"C\"                                                                                                    \n\
{                                                                                                               \n\
    /*                                                                                                          \n\
     * mach defines                                                                                             \n\
     */                                                                                                         \n\
                                                                                                                \n\
    typedef unsigned int uint32_t;                                                                              \n\
    typedef unsigned long long uint64_t;                                                                        \n\
    typedef uint32_t mach_port_t;                                                                               \n\
    typedef mach_port_t vm_map_t;                                                                               \n\
    typedef int kern_return_t;                                                                                  \n\
    typedef uint64_t mach_vm_address_t;                                                                         \n\
    typedef uint64_t mach_vm_size_t;                                                                            \n\
                                                                                                                \n\
    mach_port_t mach_task_self ();                                                                              \n\
    kern_return_t mach_vm_deallocate (vm_map_t target, mach_vm_address_t address, mach_vm_size_t size);         \n\
                                                                                                                \n\
    /*                                                                                                          \n\
     * libBacktraceRecording defines                                                                            \n\
     */                                                                                                         \n\
                                                                                                                \n\
    typedef uint32_t queue_list_scope_t;                                                                        \n\
    typedef void *dispatch_queue_t;                                                                             \n\
    typedef void *introspection_dispatch_queue_info_t;                                                          \n\
    typedef void *introspection_dispatch_item_info_ref;                                                         \n\
                                                                                                                \n\
    extern uint64_t __introspection_dispatch_queue_item_get_info (introspection_dispatch_item_info_ref item_info_ref, \n\
                                                 introspection_dispatch_item_info_ref *returned_queues_buffer,  \n\
                                                 uint64_t *returned_queues_buffer_size);                        \n\
    extern int printf(const char *format, ...);                                                                 \n\
                                                                                                                \n\
    /*                                                                                                          \n\
     * return type define                                                                                       \n\
     */                                                                                                         \n\
                                                                                                                \n\
    struct get_item_info_return_values                                                                      \n\
    {                                                                                                           \n\
        uint64_t item_info_buffer_ptr;    /* the address of the items buffer from libBacktraceRecording */  \n\
        uint64_t item_info_buffer_size;   /* the size of the items buffer from libBacktraceRecording */     \n\
    };                                                                                                          \n\
                                                                                                                \n\
    void  __lldb_backtrace_recording_get_item_info                                                          \n\
                                               (struct get_item_info_return_values *return_buffer,          \n\
                                                int debug,                                                      \n\
                                                uint64_t /* introspection_dispatch_item_info_ref item_info_ref */ item, \n\
                                                void *page_to_free,                                             \n\
                                                uint64_t page_to_free_size)                                     \n\
{                                                                                                               \n\
    if (debug)                                                                                                  \n\
      printf (\"entering get_item_info with args return_buffer == %p, debug == %d, item == 0x%llx, page_to_free == %p, page_to_free_size == 0x%llx\\n\", return_buffer, debug, item, page_to_free, page_to_free_size); \n\
    if (page_to_free != 0)                                                                                      \n\
    {                                                                                                           \n\
        mach_vm_deallocate (mach_task_self(), (mach_vm_address_t) page_to_free, (mach_vm_size_t) page_to_free_size); \n\
    }                                                                                                           \n\
                                                                                                                \n\
    __introspection_dispatch_queue_item_get_info ((void*) item,                                                 \n\
                                                  (void**)&return_buffer->item_info_buffer_ptr,                 \n\
                                                  &return_buffer->item_info_buffer_size);                       \n\
}                                                                                                               \n\
}                                                                                                               \n\
";

AppleGetItemInfoHandler::AppleGetItemInfoHandler(Process *process)
    : m_process(process), m_get_item_info_impl_code(),
      m_get_item_info_function_mutex(),
      m_get_item_info_return_buffer_addr(LLDB_INVALID_ADDRESS),
      m_get_item_info_retbuffer_mutex() {}

AppleGetItemInfoHandler::~AppleGetItemInfoHandler() {}

void AppleGetItemInfoHandler::Detach() {

  if (m_process && m_process->IsAlive() &&
      m_get_item_info_return_buffer_addr != LLDB_INVALID_ADDRESS) {
    std::unique_lock<std::mutex> lock(m_get_item_info_retbuffer_mutex,
                                      std::defer_lock);
    lock.try_lock(); // Even if we don't get the lock, deallocate the buffer
    m_process->DeallocateMemory(m_get_item_info_return_buffer_addr);
  }
}

// Compile our __lldb_backtrace_recording_get_item_info() function (from the
// source above in g_get_item_info_function_code) if we don't find that
// function in the inferior already with USE_BUILTIN_FUNCTION defined.  (e.g.
// this would be the case for testing.)
//
// Insert the __lldb_backtrace_recording_get_item_info into the inferior
// process if needed.
//
// Write the get_item_info_arglist into the inferior's memory space to prepare
// for the call.
//
// Returns the address of the arguments written down in the inferior process,
// which can be used to make the function call.

lldb::addr_t AppleGetItemInfoHandler::SetupGetItemInfoFunction(
    Thread &thread, ValueList &get_item_info_arglist) {
  ExecutionContext exe_ctx(thread.shared_from_this());
  DiagnosticManager diagnostics;
  Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_SYSTEM_RUNTIME));
  lldb::addr_t args_addr = LLDB_INVALID_ADDRESS;
  FunctionCaller *get_item_info_caller = nullptr;

  // Scope for mutex locker:
  {
    std::lock_guard<std::mutex> guard(m_get_item_info_function_mutex);

    // First stage is to make the UtilityFunction to hold our injected
    // function:

    if (!m_get_item_info_impl_code.get()) {
      if (g_get_item_info_function_code != NULL) {
        Status error;
        m_get_item_info_impl_code.reset(
            exe_ctx.GetTargetRef().GetUtilityFunctionForLanguage(
                g_get_item_info_function_code, eLanguageTypeObjC,
                g_get_item_info_function_name, error));
        if (error.Fail()) {
          if (log)
            log->Printf("Failed to get utility function: %s.",
                        error.AsCString());
          return args_addr;
        }

        if (!m_get_item_info_impl_code->Install(diagnostics, exe_ctx)) {
          if (log) {
            log->Printf("Failed to install get-item-info introspection.");
            diagnostics.Dump(log);
          }
          m_get_item_info_impl_code.reset();
          return args_addr;
        }
      } else {
        if (log)
          log->Printf("No get-item-info introspection code found.");
        return LLDB_INVALID_ADDRESS;
      }

      // Next make the runner function for our implementation utility function.
      Status error;

      TypeSystem *type_system =
          thread.GetProcess()->GetTarget().GetScratchTypeSystemForLanguage(
              nullptr, eLanguageTypeC);
      CompilerType get_item_info_return_type =
          type_system->GetBasicTypeFromAST(eBasicTypeVoid).GetPointerType();

      get_item_info_caller = m_get_item_info_impl_code->MakeFunctionCaller(
          get_item_info_return_type, get_item_info_arglist,
          thread.shared_from_this(), error);
      if (error.Fail() || get_item_info_caller == nullptr) {
        if (log)
          log->Printf("Error Inserting get-item-info function: \"%s\".",
                      error.AsCString());
        return args_addr;
      }
    } else {
      // If it's already made, then we can just retrieve the caller:
      get_item_info_caller = m_get_item_info_impl_code->GetFunctionCaller();
      if (!get_item_info_caller) {
        if (log)
          log->Printf("Failed to get get-item-info introspection caller.");
        m_get_item_info_impl_code.reset();
        return args_addr;
      }
    }
  }

  diagnostics.Clear();

  // Now write down the argument values for this particular call.  This looks
  // like it might be a race condition if other threads were calling into here,
  // but actually it isn't because we allocate a new args structure for this
  // call by passing args_addr = LLDB_INVALID_ADDRESS...

  if (!get_item_info_caller->WriteFunctionArguments(
          exe_ctx, args_addr, get_item_info_arglist, diagnostics)) {
    if (log) {
      log->Printf("Error writing get-item-info function arguments.");
      diagnostics.Dump(log);
    }

    return args_addr;
  }

  return args_addr;
}

AppleGetItemInfoHandler::GetItemInfoReturnInfo
AppleGetItemInfoHandler::GetItemInfo(Thread &thread, uint64_t item,
                                     addr_t page_to_free,
                                     uint64_t page_to_free_size,
                                     Status &error) {
  lldb::StackFrameSP thread_cur_frame = thread.GetStackFrameAtIndex(0);
  ProcessSP process_sp(thread.CalculateProcess());
  TargetSP target_sp(thread.CalculateTarget());
  ClangASTContext *clang_ast_context = target_sp->GetScratchClangASTContext();
  Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_SYSTEM_RUNTIME));

  GetItemInfoReturnInfo return_value;
  return_value.item_buffer_ptr = LLDB_INVALID_ADDRESS;
  return_value.item_buffer_size = 0;

  error.Clear();

  if (thread.SafeToCallFunctions() == false) {
    if (log)
      log->Printf("Not safe to call functions on thread 0x%" PRIx64,
                  thread.GetID());
    error.SetErrorString("Not safe to call functions on this thread.");
    return return_value;
  }

  // Set up the arguments for a call to

  // struct get_item_info_return_values
  // {
  //     uint64_t item_info_buffer_ptr;    /* the address of the items buffer
  //     from libBacktraceRecording */
  //     uint64_t item_info_buffer_size;   /* the size of the items buffer from
  //     libBacktraceRecording */
  // };
  //
  // void  __lldb_backtrace_recording_get_item_info
  //                                            (struct
  //                                            get_item_info_return_values
  //                                            *return_buffer,
  //                                             int debug,
  //                                             uint64_t item,
  //                                             void *page_to_free,
  //                                             uint64_t page_to_free_size)

  // Where the return_buffer argument points to a 24 byte region of memory
  // already allocated by lldb in the inferior process.

  CompilerType clang_void_ptr_type =
      clang_ast_context->GetBasicType(eBasicTypeVoid).GetPointerType();
  Value return_buffer_ptr_value;
  return_buffer_ptr_value.SetValueType(Value::eValueTypeScalar);
  return_buffer_ptr_value.SetCompilerType(clang_void_ptr_type);

  CompilerType clang_int_type = clang_ast_context->GetBasicType(eBasicTypeInt);
  Value debug_value;
  debug_value.SetValueType(Value::eValueTypeScalar);
  debug_value.SetCompilerType(clang_int_type);

  CompilerType clang_uint64_type =
      clang_ast_context->GetBasicType(eBasicTypeUnsignedLongLong);
  Value item_value;
  item_value.SetValueType(Value::eValueTypeScalar);
  item_value.SetCompilerType(clang_uint64_type);

  Value page_to_free_value;
  page_to_free_value.SetValueType(Value::eValueTypeScalar);
  page_to_free_value.SetCompilerType(clang_void_ptr_type);

  Value page_to_free_size_value;
  page_to_free_size_value.SetValueType(Value::eValueTypeScalar);
  page_to_free_size_value.SetCompilerType(clang_uint64_type);

  std::lock_guard<std::mutex> guard(m_get_item_info_retbuffer_mutex);
  if (m_get_item_info_return_buffer_addr == LLDB_INVALID_ADDRESS) {
    addr_t bufaddr = process_sp->AllocateMemory(
        32, ePermissionsReadable | ePermissionsWritable, error);
    if (!error.Success() || bufaddr == LLDB_INVALID_ADDRESS) {
      if (log)
        log->Printf("Failed to allocate memory for return buffer for get "
                    "current queues func call");
      return return_value;
    }
    m_get_item_info_return_buffer_addr = bufaddr;
  }

  ValueList argument_values;

  return_buffer_ptr_value.GetScalar() = m_get_item_info_return_buffer_addr;
  argument_values.PushValue(return_buffer_ptr_value);

  debug_value.GetScalar() = 0;
  argument_values.PushValue(debug_value);

  item_value.GetScalar() = item;
  argument_values.PushValue(item_value);

  if (page_to_free != LLDB_INVALID_ADDRESS)
    page_to_free_value.GetScalar() = page_to_free;
  else
    page_to_free_value.GetScalar() = 0;
  argument_values.PushValue(page_to_free_value);

  page_to_free_size_value.GetScalar() = page_to_free_size;
  argument_values.PushValue(page_to_free_size_value);

  addr_t args_addr = SetupGetItemInfoFunction(thread, argument_values);

  DiagnosticManager diagnostics;
  ExecutionContext exe_ctx;
  EvaluateExpressionOptions options;
  options.SetUnwindOnError(true);
  options.SetIgnoreBreakpoints(true);
  options.SetStopOthers(true);
  options.SetTimeout(std::chrono::milliseconds(500));
  options.SetTryAllThreads(false);
  thread.CalculateExecutionContext(exe_ctx);

  if (!m_get_item_info_impl_code) {
    error.SetErrorString("Unable to compile function to call "
                         "__introspection_dispatch_queue_item_get_info");
    return return_value;
  }

  ExpressionResults func_call_ret;
  Value results;
  FunctionCaller *func_caller = m_get_item_info_impl_code->GetFunctionCaller();
  if (!func_caller) {
    if (log)
      log->Printf("Could not retrieve function caller for "
                  "__introspection_dispatch_queue_item_get_info.");
    error.SetErrorString("Could not retrieve function caller for "
                         "__introspection_dispatch_queue_item_get_info.");
    return return_value;
  }

  func_call_ret = func_caller->ExecuteFunction(exe_ctx, &args_addr, options,
                                               diagnostics, results);
  if (func_call_ret != eExpressionCompleted || !error.Success()) {
    if (log)
      log->Printf("Unable to call "
                  "__introspection_dispatch_queue_item_get_info(), got "
                  "ExpressionResults %d, error contains %s",
                  func_call_ret, error.AsCString(""));
    error.SetErrorString("Unable to call "
                         "__introspection_dispatch_queue_get_item_info() for "
                         "list of queues");
    return return_value;
  }

  return_value.item_buffer_ptr = m_process->ReadUnsignedIntegerFromMemory(
      m_get_item_info_return_buffer_addr, 8, LLDB_INVALID_ADDRESS, error);
  if (!error.Success() ||
      return_value.item_buffer_ptr == LLDB_INVALID_ADDRESS) {
    return_value.item_buffer_ptr = LLDB_INVALID_ADDRESS;
    return return_value;
  }

  return_value.item_buffer_size = m_process->ReadUnsignedIntegerFromMemory(
      m_get_item_info_return_buffer_addr + 8, 8, 0, error);

  if (!error.Success()) {
    return_value.item_buffer_ptr = LLDB_INVALID_ADDRESS;
    return return_value;
  }
  if (log)
    log->Printf("AppleGetItemInfoHandler called "
                "__introspection_dispatch_queue_item_get_info (page_to_free == "
                "0x%" PRIx64 ", size = %" PRId64
                "), returned page is at 0x%" PRIx64 ", size %" PRId64,
                page_to_free, page_to_free_size, return_value.item_buffer_ptr,
                return_value.item_buffer_size);

  return return_value;
}
