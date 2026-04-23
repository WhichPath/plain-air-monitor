#pragma once

#include <Arduino.h>

namespace ErrorHandler {

enum class ErrorCode {
    Success = 0,
    SensorInitFailed,
    SensorReadFailed,
    SensorRecoveryFailed,
    DisplayInitFailed,
    HardwareInitFailed,
    InvalidParameter,
    Timeout,
    UnknownError,
};

struct ErrorInfo {
    ErrorCode code;
    const char* message;
    unsigned long timestampMs;
};

class ErrorManager {
public:
    static void log(ErrorCode code, const char* message = nullptr);
    static const ErrorInfo& last();
    static void clear();
    static const char* messageFor(ErrorCode code);
    static bool hasError();

private:
    static ErrorInfo s_lastError;
};

}  // namespace ErrorHandler
