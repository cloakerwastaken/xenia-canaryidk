/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/messenger_app.h"

#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

MessengerApp::MessengerApp(KernelState* kernel_state)
    : App(kernel_state, 0xF7) {}

X_RESULT MessengerApp::DispatchMessageSync(uint32_t message,
                                           uint32_t buffer_ptr,
                                           uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00200002: {
      // Used on start in blades dashboard v5759 (marketplace update) and 6717
      XELOGD("MessengerUnk200002({:08X}, {:08X}), unimplemented", buffer_ptr,
             buffer_length);
      return X_E_FAIL;
    }
    case 0x00200018: {
      // Used on logging out in blades 6717
      XELOGD("MessengerUnk200018({:08X}, {:08X}), unimplemented", buffer_ptr,
             buffer_length);
      return X_E_FAIL;
    }
  }
  XELOGE(
      "Unimplemented Messenger message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_STATUS_UNSUCCESSFUL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
