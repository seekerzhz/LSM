// Status 实现（课程提供，完整）
#include "lsm/status.h"

namespace lsm {

std::string Status::ToString() const {
  const char* type = "OK";
  switch (code_) {
    case Code::kOk:              type = "OK";              break;
    case Code::kNotFound:        type = "NotFound";        break;
    case Code::kCorruption:      type = "Corruption";      break;
    case Code::kIOError:         type = "IOError";         break;
    case Code::kInvalidArgument: type = "InvalidArgument"; break;
  }
  if (msg_.empty()) return type;
  return std::string(type) + ": " + msg_;
}

}  // namespace lsm
