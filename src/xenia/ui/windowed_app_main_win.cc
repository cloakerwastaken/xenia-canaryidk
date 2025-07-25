/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>
#include <memory>

#include "xenia/base/console.h"
#include "xenia/base/cvar.h"
#include "xenia/base/main_win.h"
#include "xenia/base/platform_win.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_win.h"

// Autogenerated by `xb premake`.
#include "build/version.h"

DEFINE_bool(enable_console, false, "Open a console window with the main window",
            "Logging");

static uintptr_t g_xenia_exe_base = 0;
static size_t g_xenia_exe_size = 0;
#if XE_ARCH_AMD64 == 1
DEFINE_bool(enable_rdrand_ntdll_patch, true,
            "Hot-patches ntdll at the start of the process to not use rdrand "
            "as part of the RNG for heap randomization. Can reduce CPU usage "
            "significantly, but is untested on all Windows versions.",
            "Win32");

// begin ntdll hack
#include <psapi.h>
static bool g_didfailtowrite = false;
static void write_process_memory(HANDLE process, uintptr_t offset,
                                 unsigned size, const unsigned char* bvals) {
  if (!WriteProcessMemory(process, (void*)offset, bvals, size, nullptr)) {
    if (!g_didfailtowrite) {
      MessageBoxA(nullptr, "Failed to write to process!", "Failed", MB_OK);
      g_didfailtowrite = true;
    }
  }
}

static constexpr unsigned char pattern_cmp_processorfeature_28_[] = {
    0x80, 0x3C, 0x25, 0x90,
    0x02, 0xFE, 0x7F, 0x00};  // cmp     byte ptr ds:7FFE0290h, 0
static constexpr unsigned char pattern_replacement[] = {
    0x48, 0x39, 0xe4,             // cmp rsp, rsp = always Z
    0x0F, 0x1F, 0x44, 0x00, 0x00  // 5byte nop
};
static void patch_ntdll_instance(HANDLE process, uintptr_t ntdll_base) {
  MODULEINFO modinfo;

  GetModuleInformation(process, (HMODULE)ntdll_base, &modinfo,
                       sizeof(MODULEINFO));

  std::vector<uintptr_t> possible_places{};

  unsigned char* strt = (unsigned char*)modinfo.lpBaseOfDll;

  for (unsigned i = 0; i < modinfo.SizeOfImage; ++i) {
    for (unsigned j = 0; j < sizeof(pattern_cmp_processorfeature_28_); ++j) {
      if (strt[i + j] != pattern_cmp_processorfeature_28_[j]) {
        goto miss;
      }
    }
    possible_places.push_back((uintptr_t)(&strt[i]));
  miss:;
  }

  for (auto&& place : possible_places) {
    write_process_memory(process, place, sizeof(pattern_replacement),
                         pattern_replacement);
  }
}

static void do_ntdll_hack_this_process() {
  patch_ntdll_instance(GetCurrentProcess(),
                       (uintptr_t)GetModuleHandleA("ntdll.dll"));
}
#endif

static HMODULE probe_for_module(void* addr) {
  // get 65k aligned addr downwards to probe for MZ
  uintptr_t base = reinterpret_cast<uintptr_t>(addr) & ~0xFFFFULL;

  constexpr unsigned max_search_iters =
      (64 * (1024 * 1024)) /
      65536;  // search down at most 64 mb (we do it in
              // batches of 64k so its pretty quick). i think its reasonable to
              // expect no module will be > 64mb
  // break if access violation thrown, we're definitely not a PE in that case
  __try {
    for (unsigned i = 0; i < max_search_iters; ++i) {
      if (*reinterpret_cast<unsigned short*>(base) == 'ZM') {
        return reinterpret_cast<HMODULE>(base);
      } else {
        base -= 65536;
      }
    }
    return nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// end ntdll hack

static constexpr auto XENIA_ERROR_LANGUAGE =
    MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT);

struct HostExceptionReport {
  _EXCEPTION_POINTERS* const ExceptionInfo;
  size_t Report_Scratchpos;

  const DWORD last_win32_error;
  const NTSTATUS last_ntstatus;

  const int errno_value;
  char Report_Scratchbuffer[2048];

  unsigned int address_format_ring_index;

  char formatted_addresses[16][128];

  void AddString(const char* s);
  static char* ChompNewlines(char* s);

  HostExceptionReport(_EXCEPTION_POINTERS* _ExceptionInfo)
      : ExceptionInfo(_ExceptionInfo),
        Report_Scratchpos(0u),
        last_win32_error(GetLastError()),
        last_ntstatus(__readgsdword(0x1250)),
        errno_value(errno),
        address_format_ring_index(0)

  {
    memset(Report_Scratchbuffer, 0, sizeof(Report_Scratchbuffer));
  }

  void DisplayExceptionMessage() {
    MessageBoxA(nullptr, Report_Scratchbuffer, "Unhandled Exception in Xenia",
                MB_ICONERROR);
  }

  const char* GetFormattedAddress(uintptr_t address);

  const char* GetFormattedAddress(PVOID address) {
    return GetFormattedAddress(reinterpret_cast<uintptr_t>(address));
  }
};
char* HostExceptionReport::ChompNewlines(char* s) {
  if (!s) {
    return nullptr;
  }
  unsigned read_pos = 0;
  unsigned write_pos = 0;

  while (true) {
    char current = s[read_pos++];
    if (current == '\n') {
      continue;
    }
    s[write_pos++] = current;
    if (!current) {
      break;
    }
  }
  return s;
}
void HostExceptionReport::AddString(const char* s) {
  size_t ln = strlen(s);

  for (size_t i = 0; i < ln; ++i) {
    Report_Scratchbuffer[i + Report_Scratchpos] = s[i];
  }
  Report_Scratchpos += ln;
}

const char* HostExceptionReport::GetFormattedAddress(uintptr_t address) {
  char(&current_buffer)[128] =
      formatted_addresses[address_format_ring_index++ % 16];

  /* if (address >= g_xenia_exe_base &&
       address - g_xenia_exe_base < g_xenia_exe_size) {
     uintptr_t offset = address - g_xenia_exe_base;

     sprintf_s(current_buffer, "xenia_canary.exe+%llX", offset);
   } else */
  {
    HMODULE hmod_for = probe_for_module((void*)address);

    if (hmod_for) {
      // get the module filename, then chomp off all but the actual file name
      // (full path is obtained)
      char tmp_module_name[MAX_PATH + 1];
      GetModuleFileNameA(hmod_for, tmp_module_name, sizeof(tmp_module_name));

      size_t search_back = strlen(tmp_module_name);
      // hunt backwards for the last sep
      while (tmp_module_name[--search_back] != '\\');

      // MessageBoxA(nullptr, tmp_module_name, "ffds", MB_OK);
      sprintf_s(current_buffer, "%s+%llX", tmp_module_name + search_back + 1,
                address - reinterpret_cast<uintptr_t>(hmod_for));

    } else {
      sprintf_s(current_buffer, "0x%llX", address);
    }
  }
  return current_buffer;
}
using ExceptionInfoCategoryHandler = bool (*)(HostExceptionReport* report);
static char* Ntstatus_msg(NTSTATUS status) {
  char* statusmsg = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 GetModuleHandleA("ntdll.dll"), status, XENIA_ERROR_LANGUAGE,
                 (LPSTR)&statusmsg, 0, NULL);
  return statusmsg;
}
static bool exception_pointers_handler(HostExceptionReport* report) {
  PVOID exception_addr =
      report->ExceptionInfo->ExceptionRecord->ExceptionAddress;

  DWORD64 last_stackpointer = report->ExceptionInfo->ContextRecord->Rsp;

  DWORD64 last_rip = report->ExceptionInfo->ContextRecord->Rip;
  DWORD except_code = report->ExceptionInfo->ExceptionRecord->ExceptionCode;

  std::string build = (
#ifdef XE_BUILD_IS_PR
      "PR#" XE_BUILD_PR_NUMBER " " XE_BUILD_PR_REPO " " XE_BUILD_PR_BRANCH
      "@" XE_BUILD_PR_COMMIT_SHORT " against "
#endif
      XE_BUILD_BRANCH "@" XE_BUILD_COMMIT_SHORT " on " XE_BUILD_DATE);

  const std::string except_message = fmt::format(
      "Exception encountered!\nBuild: {}\nException address: "
      "{}\nStackpointer: "
      "{}\nInstruction pointer: {}\nExceptionCode: 0x{} ({})\n",
      build.c_str(), report->GetFormattedAddress(exception_addr),
      report->GetFormattedAddress(last_stackpointer),
      report->GetFormattedAddress(last_rip), except_code,
      HostExceptionReport::ChompNewlines(Ntstatus_msg(except_code)));

  report->AddString(except_message.c_str());

  return true;
}

static bool exception_win32_error_handle(HostExceptionReport* report) {
  if (!report->last_win32_error) {
    return false;  // no error, nothing to do
  }
  // todo: formatmessage
  char win32_error_buf[512];
  // its ok if we dont free statusmsg, we're exiting anyway
  char* statusmsg = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, report->last_win32_error, XENIA_ERROR_LANGUAGE,
                 (LPSTR)&statusmsg, 0, nullptr);
  sprintf_s(win32_error_buf, "Last Win32 Error: 0x%X (%s)\n",
            report->last_win32_error,
            HostExceptionReport::ChompNewlines(statusmsg));
  report->AddString(win32_error_buf);
  return true;
}
static bool exception_ntstatus_error_handle(HostExceptionReport* report) {
  if (!report->last_ntstatus) {
    return false;
  }
  // todo: formatmessage
  char win32_error_buf[512];

  sprintf_s(win32_error_buf, "Last NTSTATUS: 0x%X (%s)\n",
            report->last_ntstatus, Ntstatus_msg(report->last_ntstatus));
  report->AddString(win32_error_buf);
  return true;
}

static bool exception_cerror_handle(HostExceptionReport* report) {
  if (!report->errno_value) {
    return false;
  }
  char errno_buffer[512];
  sprintf_s(errno_buffer, "Last errno value: 0x%X (%s)\n", report->errno_value,
            strerror(report->errno_value));

  report->AddString(errno_buffer);
  return true;
}

static bool thread_name_handle(HostExceptionReport* report) {
  // ll GetThreadDescription(HANDLE hThread, PWSTR *ppszThreadDescription)

  FARPROC description_getter =
      GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");

  if (!description_getter) {
    return false;
  }
  PWSTR descr = nullptr;

  reinterpret_cast<HRESULT (*)(HANDLE, PWSTR*)>(description_getter)(
      GetCurrentThread(), &descr);

  if (!descr) {
    return false;
  }

  char result_buffer[512];

  sprintf_s(result_buffer, "Faulting thread name: %ws\n", descr);

  report->AddString(result_buffer);
  return true;
}
static ExceptionInfoCategoryHandler host_exception_category_handlers[] = {
    exception_pointers_handler, exception_win32_error_handle,
    exception_ntstatus_error_handle, exception_cerror_handle,
    thread_name_handle};

LONG _UnhandledExceptionFilter(_EXCEPTION_POINTERS* ExceptionInfo) {
  HostExceptionReport report{ExceptionInfo};
  for (auto&& handler : host_exception_category_handlers) {
    __try {
      if (!handler(&report)) {
        continue;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      report.AddString("<Nested Exception Encountered>\n");
    }
  }
  report.DisplayExceptionMessage();

  return EXCEPTION_CONTINUE_SEARCH;
}
int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE hinstance_prev,
                    LPWSTR command_line, int show_cmd) {
  MODULEINFO modinfo;

  GetModuleInformation(GetCurrentProcess(), (HMODULE)hinstance, &modinfo,
                       sizeof(MODULEINFO));

  g_xenia_exe_base = reinterpret_cast<uintptr_t>(hinstance);
  g_xenia_exe_size = modinfo.SizeOfImage;

  int result;
  SetUnhandledExceptionFilter(_UnhandledExceptionFilter);
  {
    xe::ui::Win32WindowedAppContext app_context(hinstance, show_cmd);
    // TODO(Triang3l): Initialize creates a window. Set DPI awareness via the
    // manifest.
    if (!app_context.Initialize()) {
      return EXIT_FAILURE;
    }

    std::unique_ptr<xe::ui::WindowedApp> app =
        xe::ui::GetWindowedAppCreator()(app_context);

    if (!xe::ParseWin32LaunchArguments(false, app->GetPositionalOptionsUsage(),
                                       app->GetPositionalOptions(), nullptr)) {
      return EXIT_FAILURE;
    }

    // Initialize COM on the UI thread with the apartment-threaded concurrency
    // model, so dialogs can be used.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      return EXIT_FAILURE;
    }

    xe::InitializeWin32App(app->GetName());

    if (app->OnInitialize()) {
#if XE_ARCH_AMD64 == 1
      if (cvars::enable_rdrand_ntdll_patch) {
        do_ntdll_hack_this_process();
      }
#endif
      // TODO(Triang3l): Rework this, need to initialize the console properly,
      // disable has_console_attached_ by default in windowed apps, and attach
      // only if needed.
      if (cvars::enable_console) {
        xe::AttachConsole();
      }
      result = app_context.RunMainMessageLoop();
    } else {
      result = EXIT_FAILURE;
    }

    app->InvokeOnDestroy();
  }

  // Logging may still be needed in the destructors.
  xe::ShutdownWin32App();

  CoUninitialize();

  return result;
}
