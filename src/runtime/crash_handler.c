#include "crash_handler.h"
#include "owned.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#ifndef STATUS_HEAP_CORRUPTION
#define STATUS_HEAP_CORRUPTION ((DWORD)0xC0000374)
#endif
#ifndef STATUS_STACK_BUFFER_OVERRUN
#define STATUS_STACK_BUFFER_OVERRUN ((DWORD)0xC0000409)
#endif
#else
#include <signal.h>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__)
#include <ucontext.h>
#endif
#endif

static const MettleCrashDebugHeader *g_runtime_debug_header = NULL;
static const MettleCrashFunctionInfo *g_runtime_debug_functions = NULL;
static size_t g_runtime_debug_function_count = 0;
static const MettleCrashLocationInfo *g_runtime_debug_locations = NULL;
static size_t g_runtime_debug_location_count = 0;
static const MettleCrashTrapSiteInfo *g_runtime_debug_trap_sites = NULL;
static size_t g_runtime_debug_trap_site_count = 0;

static MettleCrashLocationInfo *g_runtime_sorted_locations = NULL;
static size_t g_runtime_sorted_location_count = 0;

#if defined(_WIN32) || defined(_WIN64)
static volatile LONG g_runtime_debug_handler_installed = 0;
static volatile LONG g_runtime_debug_in_handler = 0;
static PVOID g_runtime_debug_vectored_handler = NULL;
#else
static volatile sig_atomic_t g_runtime_debug_handler_installed = 0;
static volatile sig_atomic_t g_runtime_debug_in_handler = 0;
#endif

static void mettle_crash_write_decimal_uintptr(uintptr_t value) {
  char buffer[32];
  size_t index = sizeof(buffer);
  buffer[--index] = '\0';

  do {
    buffer[--index] = (char)('0' + (value % 10u));
    value /= 10u;
  } while (value != 0 && index > 0);

  mettle_crash_write_stderr(buffer + index);
}

static void mettle_crash_write_decimal_uint64(uint64_t value) {
  char buffer[32];
  size_t index = sizeof(buffer);
  buffer[--index] = '\0';

  do {
    buffer[--index] = (char)('0' + (value % 10u));
    value /= 10u;
  } while (value != 0 && index > 0);

  mettle_crash_write_stderr(buffer + index);
}

static void mettle_crash_write_hex_uintptr(uintptr_t value, size_t width) {
  static const char digits[] = "0123456789ABCDEF";
  char buffer[2 + (sizeof(uintptr_t) * 2) + 1];
  size_t index = sizeof(buffer);
  buffer[--index] = '\0';

  size_t digit_count = 0;
  do {
    buffer[--index] = digits[value & 0xFu];
    value >>= 4u;
    digit_count++;
  } while ((value != 0 || digit_count < width) && index > 2);

  buffer[--index] = 'x';
  buffer[--index] = '0';
  mettle_crash_write_stderr(buffer + index);
}

static void mettle_crash_write_pointer(const void *value) {
  mettle_crash_write_hex_uintptr((uintptr_t)value, sizeof(uintptr_t) * 2);
}

static void mettle_crash_write_newline(void);

/* Heap classifier hook: --native-heap's allocator (stdlib/std/alloc.mettle)
 * registers a function here that answers whether an address lies inside a
 * quarantined (freed) heap block, returning the block's usable byte count
 * when it does and 0 otherwise. Lets the access-violation report say
 * "use-after-free" instead of printing an anonymous pointer. */
extern long long (*mettle_crash_heap_classifier)(void *address);

/* One line of insight about WHAT a faulting address is. The raw pointer
 * value rarely tells the user anything; its neighborhood usually does. */
static void mettle_crash_classify_fault_address(uintptr_t fault_address,
                                                uintptr_t stack_low,
                                                uintptr_t stack_high) {
  if (fault_address == 0) {
    return; /* the address line already says "(null pointer)" */
  }
  if (fault_address < 4096) {
    mettle_crash_write_stderr("This address is null plus offset ");
    mettle_crash_write_decimal_uintptr(fault_address);
    mettle_crash_write_stderr(
        ": a field or array access through a null pointer");
    mettle_crash_write_newline();
    return;
  }
  if (mettle_crash_heap_classifier) {
    long long freed_size = mettle_crash_heap_classifier((void *)fault_address);
    if (freed_size > 0) {
      mettle_crash_write_stderr("This address is inside a ");
      mettle_crash_write_decimal_uintptr((uintptr_t)freed_size);
      mettle_crash_write_stderr(
          "-byte heap block that was already freed: use-after-free");
      mettle_crash_write_newline();
      return;
    }
  }
  if (stack_low != 0 && stack_high > stack_low &&
      fault_address >= stack_low - 0x100000 && fault_address < stack_high) {
    mettle_crash_write_stderr(
        "This address is in this thread's stack region: likely a dangling "
        "pointer to a stack frame that no longer exists, or a stack array "
        "overrun");
    mettle_crash_write_newline();
  }
}

static void mettle_crash_write_newline(void) {
#if defined(_WIN32) || defined(_WIN64)
  mettle_crash_write_stderr("\r\n");
#else
  mettle_crash_write_stderr("\n");
#endif
}

#if defined(_WIN32) || defined(_WIN64)
const char *mettle_crash_exception_name(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
    return "access violation";
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    return "array bounds exceeded";
  case EXCEPTION_BREAKPOINT:
    return "breakpoint";
  case EXCEPTION_DATATYPE_MISALIGNMENT:
    return "datatype misalignment";
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    return "floating-point divide by zero";
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    return "illegal instruction";
  case EXCEPTION_IN_PAGE_ERROR:
    return "in-page error";
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    return "integer divide by zero";
  case EXCEPTION_INT_OVERFLOW:
    return "integer overflow";
  case EXCEPTION_STACK_OVERFLOW:
    return "stack overflow";
  case STATUS_HEAP_CORRUPTION:
    return "heap corruption";
  case STATUS_STACK_BUFFER_OVERRUN:
    return "stack buffer overrun";
  default:
    return "unknown exception";
  }
}
#else
static const char *mettle_crash_signal_name(int signo) {
  switch (signo) {
  case SIGSEGV:
    return "segmentation fault (invalid memory access)";
  case SIGBUS:
    return "bus error (misaligned or invalid memory access)";
  case SIGFPE:
    return "arithmetic exception (e.g. integer divide by zero)";
  case SIGILL:
    return "illegal instruction";
  case SIGABRT:
    return "aborted";
  default:
    return "fatal signal";
  }
}
#endif

static int mettle_crash_address_is_readable(const void *address,
                                            size_t length) {
  if (!address || length == 0) {
    return 0;
  }

#if defined(_WIN32) || defined(_WIN64)
  MEMORY_BASIC_INFORMATION info;
  if (VirtualQuery(address, &info, sizeof(info)) == 0) {
    return 0;
  }
  if (info.State != MEM_COMMIT) {
    return 0;
  }
  if ((info.Protect & PAGE_GUARD) != 0 || info.Protect == PAGE_NOACCESS) {
    return 0;
  }

  uintptr_t start = (uintptr_t)address;
  uintptr_t region_start = (uintptr_t)info.BaseAddress;
  uintptr_t region_end = region_start + info.RegionSize;
  return start >= region_start && start + length <= region_end;
#else
  return mettle_address_is_readable(address, (unsigned long long)length);
#endif
}

/* Two location records may share an address: a marker that opens a function and
 * the marker for its first statement land on the same byte when nothing is
 * emitted between them. The lookup below takes the LAST record at or before the
 * program counter, so which of the two answers depends on how the sort ordered
 * them -- and ordering equal keys is exactly what a sort is free to do as it
 * likes. glibc and the Microsoft runtime chose differently, so the same program
 * reported the faulting statement on Windows and the function's own declaration
 * on Linux.
 *
 * The order is total now: address, then the record's position in the emitted
 * table. Records are emitted in program order, so a later one describes code
 * that begins where an earlier one described none, and the later one is the
 * answer. `g_runtime_sort_base` is the array being permuted; registration runs
 * once, from startup, before anything else can be looking. */
static const MettleCrashLocationInfo *g_runtime_sort_base = NULL;

static int mettle_crash_compare_location_index(const void *left,
                                               const void *right) {
  size_t index_left = *(const size_t *)left;
  size_t index_right = *(const size_t *)right;
  uintptr_t addr_left = (uintptr_t)g_runtime_sort_base[index_left].address;
  uintptr_t addr_right = (uintptr_t)g_runtime_sort_base[index_right].address;
  if (addr_left < addr_right) {
    return -1;
  }
  if (addr_left > addr_right) {
    return 1;
  }
  if (index_left < index_right) {
    return -1;
  }
  if (index_left > index_right) {
    return 1;
  }
  return 0;
}

static void mettle_crash_release_sorted_locations(void) {
  free(g_runtime_sorted_locations);
  g_runtime_sorted_locations = NULL;
  g_runtime_sorted_location_count = 0;
}

static int mettle_crash_prepare_sorted_locations(
    const MettleCrashLocationInfo *locations, size_t location_count) {
  mettle_crash_release_sorted_locations();
  if (!locations || location_count == 0) {
    return 1;
  }

  size_t *order = (size_t *)malloc(location_count * sizeof(size_t));
  if (!order) {
    return 0;
  }
  g_runtime_sorted_locations =
      (MettleCrashLocationInfo *)malloc(location_count *
                                      sizeof(MettleCrashLocationInfo));
  if (!g_runtime_sorted_locations) {
    free(order);
    return 0;
  }
  for (size_t i = 0; i < location_count; i++) {
    order[i] = i;
  }
  g_runtime_sort_base = locations;
  qsort(order, location_count, sizeof(size_t),
        mettle_crash_compare_location_index);
  g_runtime_sort_base = NULL;
  for (size_t i = 0; i < location_count; i++) {
    g_runtime_sorted_locations[i] = locations[order[i]];
  }
  free(order);
  g_runtime_sorted_location_count = location_count;
  return 1;
}

static const MettleCrashFunctionInfo *
mettle_crash_find_function(uintptr_t program_counter) {
  for (size_t i = 0; i < g_runtime_debug_function_count; i++) {
    const MettleCrashFunctionInfo *info = &g_runtime_debug_functions[i];
    uintptr_t start = (uintptr_t)info->start_address;
    uintptr_t end = (uintptr_t)info->end_address;
    if (program_counter >= start && program_counter < end) {
      return info;
    }
  }
  return NULL;
}

static const MettleCrashLocationInfo *
mettle_crash_find_location(uintptr_t program_counter,
                           const MettleCrashFunctionInfo *function_info) {
  const MettleCrashLocationInfo *locations = g_runtime_sorted_locations
                                                 ? g_runtime_sorted_locations
                                                 : g_runtime_debug_locations;
  size_t location_count = g_runtime_sorted_location_count
                              ? g_runtime_sorted_location_count
                              : g_runtime_debug_location_count;
  uintptr_t function_start =
      function_info ? (uintptr_t)function_info->start_address : 0;
  uintptr_t function_end =
      function_info ? (uintptr_t)function_info->end_address : UINTPTR_MAX;

  if (!locations || location_count == 0) {
    return NULL;
  }

  if (g_runtime_sorted_locations) {
    size_t low = 0;
    size_t high = location_count;
    const MettleCrashLocationInfo *best = NULL;

    while (low < high) {
      size_t mid = low + (high - low) / 2;
      uintptr_t address = (uintptr_t)locations[mid].address;
      if (address < function_start) {
        low = mid + 1;
        continue;
      }
      if (address >= function_end) {
        high = mid;
        continue;
      }
      if (address <= program_counter) {
        best = &locations[mid];
        low = mid + 1;
      } else {
        high = mid;
      }
    }
    return best;
  }

  const MettleCrashLocationInfo *best = NULL;
  uintptr_t best_address = 0;
  for (size_t i = 0; i < location_count; i++) {
    const MettleCrashLocationInfo *info = &locations[i];
    uintptr_t address = (uintptr_t)info->address;
    if (address < function_start || address >= function_end) {
      continue;
    }
    if (address <= program_counter && (!best || address >= best_address)) {
      best = info;
      best_address = address;
    }
  }
  return best;
}

static const MettleCrashTrapSiteInfo *
mettle_crash_find_trap_site(uintptr_t program_counter) {
  for (size_t i = 0; i < g_runtime_debug_trap_site_count; i++) {
    const MettleCrashTrapSiteInfo *site = &g_runtime_debug_trap_sites[i];
    if ((uintptr_t)site->address == program_counter) {
      return site;
    }
  }
  return NULL;
}

static void mettle_crash_write_caret_padding(uintptr_t column) {
  size_t spaces = column > 0 ? (size_t)(column - 1) : 0;
  for (size_t i = 0; i < spaces; i++) {
    mettle_crash_write_stderr(" ");
  }
}

static void mettle_crash_write_source_snippet(
    const MettleCrashTrapSiteInfo *site, uint64_t arg0, uint64_t arg1) {
  const char *source_line = site ? site->source_line : NULL;

  if (!site || !source_line || source_line[0] == '\0') {
    return;
  }

  mettle_crash_write_stderr("   |");
  mettle_crash_write_newline();
  mettle_crash_write_decimal_uintptr(site->line);
  mettle_crash_write_stderr(" | ");
  mettle_crash_write_stderr(source_line);
  mettle_crash_write_newline();
  mettle_crash_write_stderr("   | ");
  mettle_crash_write_caret_padding(site->column);
  mettle_crash_write_stderr("^");

  if (site->kind == METTLE_CRASH_TRAP_NULL_DEREF) {
    mettle_crash_write_stderr(" null pointer dereference");
  } else if (site->kind == METTLE_CRASH_TRAP_ARRAY_BOUNDS) {
    mettle_crash_write_stderr(" index ");
    mettle_crash_write_decimal_uint64(arg0);
    mettle_crash_write_stderr(" is out of bounds (0..");
    if (arg1 > 0) {
      mettle_crash_write_decimal_uint64(arg1 - 1u);
    } else {
      mettle_crash_write_stderr("0");
    }
    mettle_crash_write_stderr(")");
  }
  mettle_crash_write_newline();
}

static void mettle_crash_write_location_arrow(
    const char *function_name, const char *filename, uintptr_t line,
    uintptr_t column) {
  mettle_crash_write_stderr("  --> ");
  if (filename && filename[0] != '\0') {
    mettle_crash_write_stderr(filename);
    mettle_crash_write_stderr(":");
    mettle_crash_write_decimal_uintptr(line);
    mettle_crash_write_stderr(":");
    mettle_crash_write_decimal_uintptr(column);
  } else {
    mettle_crash_write_stderr("<unknown>");
  }
  if (function_name && function_name[0] != '\0') {
    mettle_crash_write_stderr(" in ");
    mettle_crash_write_stderr(function_name);
  }
  mettle_crash_write_newline();
}

static void mettle_crash_write_trap_headline(uint32_t kind, const char *message,
                                             uint64_t arg0, uint64_t arg1) {
  if (kind == METTLE_CRASH_TRAP_NULL_DEREF) {
    mettle_crash_write_stderr("Fatal error: null pointer dereference");
  } else if (kind == METTLE_CRASH_TRAP_ARRAY_BOUNDS) {
    mettle_crash_write_stderr("Fatal error: array index out of bounds (index ");
    mettle_crash_write_decimal_uint64(arg0);
    mettle_crash_write_stderr(", length ");
    mettle_crash_write_decimal_uint64(arg1);
    mettle_crash_write_stderr(")");
  } else if (message && message[0] != '\0') {
    mettle_crash_write_stderr(message);
  } else {
    mettle_crash_write_stderr("Fatal runtime trap");
  }
  mettle_crash_write_newline();
}

static void mettle_crash_write_trap_report(uintptr_t program_counter,
                                           uint32_t kind, const char *message,
                                           uint64_t arg0, uint64_t arg1) {
  const MettleCrashTrapSiteInfo *site =
      mettle_crash_find_trap_site(program_counter);
  const MettleCrashFunctionInfo *function_info =
      mettle_crash_find_function(program_counter);
  const MettleCrashLocationInfo *location_info =
      mettle_crash_find_location(program_counter, function_info);
  const char *function_name = NULL;
  const char *filename = NULL;
  uintptr_t line = 0;
  uintptr_t column = 0;

  mettle_crash_write_trap_headline(site ? site->kind : kind, message, arg0, arg1);

  if (site) {
    function_name = site->function_name;
    filename = site->filename;
    line = site->line;
    column = site->column;
  } else if (location_info) {
    function_name = location_info->function_name;
    filename = location_info->filename;
    line = location_info->line;
    column = location_info->column;
  } else if (function_info) {
    function_name = function_info->function_name;
    filename = function_info->filename;
    line = function_info->line;
    column = function_info->column;
  }

  if (filename && line > 0) {
    mettle_crash_write_location_arrow(function_name, filename, line, column);
    mettle_crash_write_source_snippet(site, arg0, arg1);
    return;
  }

  /* A build without -s carries no trap sites, no line table and no function
   * table, so everything above found nothing and the whole report is one line.
   * Crash reporting is on by default, so this is what most people see, and it
   * used to say nothing about where the answer lives. */
  mettle_crash_write_stderr(
      "  no source location in this build; rebuild with -s for the file, line "
      "and stack trace");
  mettle_crash_write_newline();
}

static void mettle_crash_print_frame(size_t index, uintptr_t program_counter) {
  const MettleCrashFunctionInfo *function_info =
      mettle_crash_find_function(program_counter);
  const MettleCrashLocationInfo *location_info =
      mettle_crash_find_location(program_counter, function_info);
  const char *function_name = "<unknown>";
  const char *filename = NULL;
  uintptr_t line = 0;
  uintptr_t column = 0;

  if (location_info) {
    function_name =
        location_info->function_name ? location_info->function_name : function_name;
    filename = location_info->filename;
    line = location_info->line;
    column = location_info->column;
  } else if (function_info) {
    function_name =
        function_info->function_name ? function_info->function_name : function_name;
    filename = function_info->filename;
    line = function_info->line;
    column = function_info->column;
  }

  if (filename && line > 0) {
    mettle_crash_write_stderr("  #");
    mettle_crash_write_decimal_uintptr((uintptr_t)index);
    mettle_crash_write_stderr(" ");
    mettle_crash_write_stderr(function_name);
    mettle_crash_write_stderr(" at ");
    mettle_crash_write_stderr(filename);
    mettle_crash_write_stderr(":");
    mettle_crash_write_decimal_uintptr(line);
    mettle_crash_write_stderr(":");
    mettle_crash_write_decimal_uintptr(column);
    mettle_crash_write_stderr(" (");
    mettle_crash_write_pointer((void *)program_counter);
    mettle_crash_write_stderr(")");
    mettle_crash_write_newline();
  } else {
    mettle_crash_write_stderr("  #");
    mettle_crash_write_decimal_uintptr((uintptr_t)index);
    mettle_crash_write_stderr(" ");
    mettle_crash_write_stderr(function_name);
    mettle_crash_write_stderr(" (");
    mettle_crash_write_pointer((void *)program_counter);
    mettle_crash_write_stderr(")");
    mettle_crash_write_newline();
  }
}

/* Function records alone place a fault in a function, at the line that
 * function is declared on. Per-statement lines are a separate, larger table
 * that only -s / -d ask for, so say where the exact line is. */
static void mettle_crash_write_precision_hint(void) {
  if (g_runtime_debug_location_count > 0 ||
      g_runtime_debug_function_count == 0) {
    return;
  }
  mettle_crash_write_stderr(
      "Lines above are where each function is declared; rebuild with -s for "
      "the exact statement.");
  mettle_crash_write_newline();
}

static void mettle_crash_print_trace_from_frame(uintptr_t program_counter,
                                                uintptr_t frame_pointer) {
  mettle_crash_write_stderr("Stack trace:");
  mettle_crash_write_newline();
  mettle_crash_print_frame(0, program_counter);

  uintptr_t current_frame = frame_pointer;
  size_t index = 1;
  while (index < 32) {
    uintptr_t next_frame = 0;
    uintptr_t return_address = 0;

    if (!current_frame ||
        !mettle_crash_address_is_readable((const void *)current_frame,
                                          sizeof(uintptr_t) * 2)) {
      break;
    }

    next_frame = *((uintptr_t *)current_frame);
    return_address = *(((uintptr_t *)current_frame) + 1);

    if (next_frame <= current_frame || return_address == 0) {
      break;
    }

    /* A trap raised from a runtime helper reports the caller's address as the
     * program counter and the helper's own frame as the frame pointer, so the
     * first link in the chain is the return into that same caller: the trace
     * printed its innermost frame twice, one byte apart, on every report the
     * safety path has ever made. Recognizing it costs one comparison and
     * leaves the hardware-fault path alone, where a faulting address is never
     * equal to a return address. */
    if (index == 1 && return_address == program_counter) {
      current_frame = next_frame;
      continue;
    }

    mettle_crash_print_frame(index, return_address - 1u);
    current_frame = next_frame;
    index++;
  }
  mettle_crash_write_precision_hint();
}

#if defined(_WIN32) || defined(_WIN64)
#ifndef DBG_PRINTEXCEPTION_C
#define DBG_PRINTEXCEPTION_C ((DWORD)0x40010006u)
#endif
#ifndef DBG_PRINTEXCEPTION_WIDE_C
#define DBG_PRINTEXCEPTION_WIDE_C ((DWORD)0x4001000Au)
#endif
#ifndef MS_VC_EXCEPTION
#define MS_VC_EXCEPTION ((DWORD)0x406D1388u)
#endif

static void mettle_crash_terminate_with_code(UINT exit_code) {
  HANDLE process = GetCurrentProcess();
  TerminateProcess(process, exit_code);
}

static LONG WINAPI
mettle_crash_unhandled_exception_filter(EXCEPTION_POINTERS *exception_info) {
  if (!exception_info || !exception_info->ExceptionRecord) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  {
    DWORD code = exception_info->ExceptionRecord->ExceptionCode;
    if (code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C ||
        code == MS_VC_EXCEPTION) {
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }

  if (InterlockedExchange(&g_runtime_debug_in_handler, 1) != 0) {
    mettle_crash_terminate_with_code(1);
  }

  const EXCEPTION_RECORD *record = exception_info->ExceptionRecord;
  const CONTEXT *context = exception_info->ContextRecord;
  uintptr_t program_counter = (uintptr_t)record->ExceptionAddress;
  uintptr_t frame_pointer = context ? (uintptr_t)context->Rbp : 0;
  const MettleCrashTrapSiteInfo *trap_site =
      mettle_crash_find_trap_site(program_counter);

  mettle_crash_write_stderr("Unhandled runtime exception ");
  mettle_crash_write_hex_uintptr((uintptr_t)record->ExceptionCode, 8);
  mettle_crash_write_stderr(" (");
  mettle_crash_write_stderr(
      mettle_crash_exception_name(record->ExceptionCode));
  mettle_crash_write_stderr(")");
  mettle_crash_write_newline();

  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record->NumberParameters >= 2) {
    const char *operation = "read";
    uintptr_t fault_address =
        (uintptr_t)record->ExceptionInformation[1];
    if (record->ExceptionInformation[0] == 1u) {
      operation = "write";
    } else if (record->ExceptionInformation[0] == 8u) {
      operation = "execute";
    }
    mettle_crash_write_stderr("Attempted to ");
    mettle_crash_write_stderr(operation);
    mettle_crash_write_stderr(" inaccessible memory at ");
    mettle_crash_write_pointer((void *)fault_address);
    if (fault_address == 0) {
      mettle_crash_write_stderr(" (null pointer)");
    }
    mettle_crash_write_newline();
    {
      NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
      mettle_crash_classify_fault_address(
          fault_address, tib ? (uintptr_t)tib->StackLimit : 0,
          tib ? (uintptr_t)tib->StackBase : 0);
    }
  } else {
    mettle_crash_write_stderr("Exception address: ");
    mettle_crash_write_pointer((void *)program_counter);
    mettle_crash_write_newline();
  }

  if (trap_site) {
    mettle_crash_write_location_arrow(trap_site->function_name,
                                      trap_site->filename, trap_site->line,
                                      trap_site->column);
    mettle_crash_write_source_snippet(trap_site, 0, 0);
  } else {
    const MettleCrashFunctionInfo *function_info =
        mettle_crash_find_function(program_counter);
    const MettleCrashLocationInfo *location_info =
        mettle_crash_find_location(program_counter, function_info);
    if (location_info && location_info->filename &&
        location_info->line > 0) {
      mettle_crash_write_location_arrow(location_info->function_name,
                                        location_info->filename,
                                        location_info->line,
                                        location_info->column);
    } else if (function_info && function_info->filename &&
               function_info->line > 0) {
      mettle_crash_write_location_arrow(function_info->function_name,
                                        function_info->filename,
                                        function_info->line,
                                        function_info->column);
    }
  }

  mettle_crash_print_trace_from_frame(program_counter, frame_pointer);
  mettle_crash_terminate_with_code(1);
  return EXCEPTION_EXECUTE_HANDLER;
}
#else

static void mettle_crash_terminate_with_code(int exit_code) {
  _exit(exit_code);
}

static void mettle_crash_extract_fault_context(void *ucontext_raw,
                                               uintptr_t *out_pc,
                                               uintptr_t *out_fp) {
  *out_pc = 0;
  *out_fp = 0;
#if defined(__linux__) && defined(__x86_64__)
  ucontext_t *uc = (ucontext_t *)ucontext_raw;
  if (uc) {
    *out_pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    *out_fp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
  }
#elif defined(__APPLE__) && defined(__x86_64__)
  ucontext_t *uc = (ucontext_t *)ucontext_raw;
  if (uc && uc->uc_mcontext) {
    *out_pc = (uintptr_t)uc->uc_mcontext->__ss.__rip;
    *out_fp = (uintptr_t)uc->uc_mcontext->__ss.__rbp;
  }
#elif defined(__linux__) && defined(__aarch64__)
  ucontext_t *uc = (ucontext_t *)ucontext_raw;
  if (uc) {
    *out_pc = (uintptr_t)uc->uc_mcontext.pc;
    *out_fp = (uintptr_t)uc->uc_mcontext.regs[29];
  }
#else
  (void)ucontext_raw;
#endif
}

static void mettle_crash_crash_signal_handler(int signo, siginfo_t *info,
                                              void *ucontext_raw) {
  if (g_runtime_debug_in_handler) {
    mettle_crash_terminate_with_code(128 + signo);
  }
  g_runtime_debug_in_handler = 1;

  mettle_crash_write_stderr("Unhandled runtime signal ");
  mettle_crash_write_decimal_uintptr((uintptr_t)signo);
  mettle_crash_write_stderr(" (");
  mettle_crash_write_stderr(mettle_crash_signal_name(signo));
  mettle_crash_write_stderr(")\n");

  if (info && (signo == SIGSEGV || signo == SIGBUS)) {
    mettle_crash_write_stderr("Faulting address: ");
    mettle_crash_write_pointer(info->si_addr);
    if (info->si_addr == NULL) {
      mettle_crash_write_stderr("  (null pointer dereference)");
    }
    mettle_crash_write_stderr("\n");
    /* no portable stack bounds here: classify null+offset and freed-heap */
    mettle_crash_classify_fault_address((uintptr_t)info->si_addr, 0, 0);
  }

  uintptr_t program_counter = 0;
  uintptr_t frame_pointer = 0;
  mettle_crash_extract_fault_context(ucontext_raw, &program_counter,
                                     &frame_pointer);
  if (program_counter != 0) {
    mettle_crash_write_stderr("Fault instruction: ");
    mettle_crash_write_pointer((void *)program_counter);
    mettle_crash_write_stderr("\n");
    mettle_crash_print_trace_from_frame(program_counter, frame_pointer);
  } else {
    mettle_crash_write_stderr(
        "Stack trace unavailable (no machine context for this platform)\n");
  }

  mettle_crash_terminate_with_code(128 + signo);
}
#endif

void mettle_crash_register_image(const MettleCrashFunctionInfo *functions,
                                 size_t function_count,
                                 const MettleCrashLocationInfo *locations,
                                 size_t location_count) {
  MettleCrashDebugImage image = {0};
  static MettleCrashDebugHeader legacy_header;

  legacy_header.magic = METTLE_CRASH_DEBUG_MAGIC;
  legacy_header.version = METTLE_CRASH_DEBUG_VERSION;
  legacy_header.function_count = (uint32_t)function_count;
  legacy_header.location_count = (uint32_t)location_count;
  legacy_header.trap_site_count = 0;

  image.header = &legacy_header;
  image.functions = functions;
  image.locations = locations;
  image.trap_sites = NULL;
  mettle_crash_register_debug_image(&image);
}

void mettle_crash_register_debug_image(const MettleCrashDebugImage *image) {
  const MettleCrashDebugHeader *header = image ? image->header : NULL;
  size_t function_count = 0;
  size_t location_count = 0;
  size_t trap_site_count = 0;

  mettle_crash_release_sorted_locations();

  if (header && header->magic == METTLE_CRASH_DEBUG_MAGIC &&
      header->version == METTLE_CRASH_DEBUG_VERSION) {
    function_count = header->function_count;
    location_count = header->location_count;
    trap_site_count = header->trap_site_count;
  } else if (image) {
    function_count = image->functions ? 1 : 0;
    location_count = image->locations ? 1 : 0;
  }

  g_runtime_debug_header = header;
  g_runtime_debug_functions = image ? image->functions : NULL;
  g_runtime_debug_function_count = function_count;
  g_runtime_debug_locations = image ? image->locations : NULL;
  g_runtime_debug_location_count = location_count;
  g_runtime_debug_trap_sites = image ? image->trap_sites : NULL;
  g_runtime_debug_trap_site_count = trap_site_count;

  if (g_runtime_debug_locations && location_count > 0) {
    if (!mettle_crash_prepare_sorted_locations(g_runtime_debug_locations,
                                               location_count)) {
      g_runtime_debug_locations = image ? image->locations : NULL;
      g_runtime_sorted_locations = NULL;
      g_runtime_sorted_location_count = 0;
    }
  }
}

void mettle_crash_install(void) {
#if defined(_WIN32) || defined(_WIN64)
  if (InterlockedCompareExchange(&g_runtime_debug_handler_installed, 1, 0) == 0) {
    g_runtime_debug_vectored_handler =
        AddVectoredExceptionHandler(1, mettle_crash_unhandled_exception_filter);
    SetUnhandledExceptionFilter(mettle_crash_unhandled_exception_filter);
  }
#else
  if (g_runtime_debug_handler_installed) {
    return;
  }
  g_runtime_debug_handler_installed = 1;

  void (*handler)(int, void *, void *) =
      (void (*)(int, void *, void *))mettle_crash_crash_signal_handler;
  (void)mettle_install_signal_handler(SIGSEGV, handler);
  (void)mettle_install_signal_handler(SIGBUS, handler);
  (void)mettle_install_signal_handler(SIGFPE, handler);
  (void)mettle_install_signal_handler(SIGILL, handler);
  (void)mettle_install_signal_handler(SIGABRT, handler);
#endif
}

static void mettle_crash_trap_impl(uint32_t kind, const char *message,
                                   const void *program_counter,
                                   const void *frame_pointer, uint64_t arg0,
                                   uint64_t arg1) {
#if defined(_WIN32) || defined(_WIN64)
  if (InterlockedExchange(&g_runtime_debug_in_handler, 1) != 0) {
    mettle_crash_terminate_with_code(1);
    return;
  }
#else
  if (g_runtime_debug_in_handler) {
    mettle_crash_terminate_with_code(1);
    return;
  }
  g_runtime_debug_in_handler = 1;
#endif

  mettle_crash_write_trap_report((uintptr_t)program_counter, kind, message, arg0,
                                 arg1);
  if (program_counter || frame_pointer) {
    mettle_crash_print_trace_from_frame((uintptr_t)program_counter,
                                        (uintptr_t)frame_pointer);
  }
#if defined(_WIN32) || defined(_WIN64)
  mettle_crash_terminate_with_code(1);
#else
  mettle_crash_terminate_with_code(1);
#endif
}

void mettle_crash_trap_ex(uint32_t kind, const char *message,
                          const void *program_counter,
                          const void *frame_pointer, uint64_t arg0,
                          uint64_t arg1) {
  mettle_crash_trap_impl(kind, message, program_counter, frame_pointer, arg0,
                         arg1);
}

void mettle_crash_trap(const char *message, const void *program_counter,
                      const void *frame_pointer) {
  mettle_crash_trap_impl(METTLE_CRASH_TRAP_UNKNOWN, message, program_counter,
                         frame_pointer, 0, 0);
}
