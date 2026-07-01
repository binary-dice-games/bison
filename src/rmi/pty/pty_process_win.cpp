// MIT License © 2025 Binary Dice Games
/**
 * @file pty_process_win.cpp
 * @brief Windows PTY process implementation using ConPTY (CreatePseudoConsole).
 */
#ifdef _WIN32

#include "src/rmi/pty/pty_process.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

// ConPTY API — requires Windows 10 1809 or later.
#include <processthreadsapi.h>

namespace bdg {
namespace bison {
namespace rmi {
namespace pty {

struct pty_process::impl {
  HANDLE h_out_read{INVALID_HANDLE_VALUE}; ///< Read child output.
  HANDLE h_in_write{INVALID_HANDLE_VALUE}; ///< Write to child input.
  HANDLE h_process{INVALID_HANDLE_VALUE};
  HPCON hpc{INVALID_HANDLE_VALUE};
  std::vector<uint8_t> attr_list_buf;

  ~impl() {
    if (h_out_read != INVALID_HANDLE_VALUE)
      CloseHandle(h_out_read);
    if (h_in_write != INVALID_HANDLE_VALUE)
      CloseHandle(h_in_write);
    if (hpc != INVALID_HANDLE_VALUE)
      ClosePseudoConsole(hpc);
    if (h_process != INVALID_HANDLE_VALUE)
      CloseHandle(h_process);
  }
};

pty_process::pty_process(pty_config cfg) : impl_(std::make_unique<impl>()) {
  // Create pipe pair for reading child stdout (parent reads from h_out_read).
  HANDLE h_out_write = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&impl_->h_out_read, &h_out_write, nullptr, 0))
    throw std::runtime_error("pty_process: CreatePipe (output) failed");

  // Create pipe pair for writing to child stdin (parent writes to h_in_write).
  HANDLE h_in_read = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&h_in_read, &impl_->h_in_write, nullptr, 0)) {
    CloseHandle(h_out_write);
    throw std::runtime_error("pty_process: CreatePipe (input) failed");
  }

  // Allocate the ConPTY using the child-side pipe ends.
  COORD size{static_cast<SHORT>(cfg.cols), static_cast<SHORT>(cfg.rows)};
  HRESULT hr = CreatePseudoConsole(size, h_in_read, h_out_write, 0, &impl_->hpc);
  CloseHandle(h_in_read);
  CloseHandle(h_out_write);
  if (FAILED(hr))
    throw std::runtime_error("pty_process: CreatePseudoConsole failed");

  // Build STARTUPINFOEX with the ConPTY attribute.
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  impl_->attr_list_buf.resize(attr_size);
  auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(impl_->attr_list_buf.data());
  if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size))
    throw std::runtime_error("pty_process: InitializeProcThreadAttributeList failed");
  if (!UpdateProcThreadAttribute(attr_list, 0,
                                 PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                 impl_->hpc, sizeof(HPCON),
                                 nullptr, nullptr)) {
    DeleteProcThreadAttributeList(attr_list);
    throw std::runtime_error("pty_process: UpdateProcThreadAttribute failed");
  }

  // Build the command line string.
  std::string cmdline = cfg.cmd;
  for (const auto& a : cfg.args) {
    cmdline += ' ';
    cmdline += a;
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
  si.lpAttributeList = attr_list;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = nullptr;
  si.StartupInfo.hStdOutput = nullptr;
  si.StartupInfo.hStdError = nullptr;

  // Convert cmdline to wide string.
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), -1, nullptr, 0);
  std::vector<wchar_t> wcmd(static_cast<size_t>(wlen));
  MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), -1, wcmd.data(), wlen);

  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(
      nullptr, wcmd.data(), nullptr, nullptr,
      FALSE,
      EXTENDED_STARTUPINFO_PRESENT,
      nullptr, nullptr,
      &si.StartupInfo, &pi);

  DeleteProcThreadAttributeList(attr_list);

  if (!ok)
    throw std::runtime_error("pty_process: CreateProcess failed");

  CloseHandle(pi.hThread);
  impl_->h_process = pi.hProcess;
}

pty_process::~pty_process() = default;

pty_process::pty_process(pty_process&&) noexcept = default;
pty_process& pty_process::operator=(pty_process&&) noexcept = default;

HANDLE pty_process::h_out_read() const noexcept {
  return impl_ ? impl_->h_out_read : INVALID_HANDLE_VALUE;
}

HANDLE pty_process::h_in_write() const noexcept {
  return impl_ ? impl_->h_in_write : INVALID_HANDLE_VALUE;
}

void pty_process::release_handles(HANDLE& out_read, HANDLE& in_write) noexcept {
  if (!impl_) {
    out_read = INVALID_HANDLE_VALUE;
    in_write = INVALID_HANDLE_VALUE;
    return;
  }
  out_read = impl_->h_out_read;
  in_write = impl_->h_in_write;
  impl_->h_out_read = INVALID_HANDLE_VALUE;
  impl_->h_in_write = INVALID_HANDLE_VALUE;
}

int pty_process::wait() {
  if (!impl_ || impl_->h_process == INVALID_HANDLE_VALUE)
    return -1;
  WaitForSingleObject(impl_->h_process, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(impl_->h_process, &code);
  CloseHandle(impl_->h_process);
  impl_->h_process = INVALID_HANDLE_VALUE;
  return static_cast<int>(code);
}

void pty_process::terminate() {
  if (impl_ && impl_->h_process != INVALID_HANDLE_VALUE)
    TerminateProcess(impl_->h_process, 1);
}

} // namespace pty
} // namespace rmi
} // namespace bison
} // namespace bdg

#endif // _WIN32
