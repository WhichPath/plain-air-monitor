#include "core/SensorManager.h"

#include <cstdio>

#include "config/AppConfig.h"
#include "utils/ErrorHandler.h"

namespace Sensor {

SensorManager::SensorManager()
    : m_state(SensorState::Uninitialized), m_errorCount(0) {
}

SensorManager::~SensorManager() {
    stopMeasurement();
}

bool SensorManager::begin() {
    Serial.println("Initializing SPS30 sensor...");
    m_state = SensorState::Initializing;

    int16_t rc = sensirion_uart_hal_init(nullptr);
    if (rc != 0) {
        m_state = SensorState::Error;
        ErrorHandler::ErrorManager::log(ErrorHandler::ErrorCode::SensorInitFailed,
                                        "UART HAL initialization failed");
        Serial.printf("UART HAL initialization failed: %d\n", rc);
        return false;
    }

    for (uint8_t attempt = 0; attempt < AppConfig::Timing::kSensorReadRetries; ++attempt) {
        rc = sps30_wake_up_sequence();
        if (rc == 0) {
            break;
        }
        delay(200);
    }

    if (rc != 0) {
        m_state = SensorState::Error;
        ErrorHandler::ErrorManager::log(ErrorHandler::ErrorCode::SensorInitFailed,
                                        "Sensor wake-up failed");
        Serial.printf("Sensor wake-up failed: %s\n", shdlcErrorToString(rc).c_str());
        return false;
    }

    rc = sps30_start_measurement(
        static_cast<sps30_output_format>(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT));
    if (rc != 0) {
        m_state = SensorState::Error;
        ErrorHandler::ErrorManager::log(ErrorHandler::ErrorCode::SensorInitFailed,
                                        "Failed to start measurement");
        Serial.printf("Failed to start measurement: %s\n", shdlcErrorToString(rc).c_str());
        return false;
    }

    m_errorCount = 0;
    m_state = SensorState::Measuring;
    Serial.println("SPS30 sensor initialized");
    return true;
}

bool SensorManager::readData(SensorData& data) {
    if (m_state != SensorState::Measuring) {
        return false;
    }

    const int16_t rc = sps30_read_measurement_values_float(
        &data.pm1, &data.pm2_5, &data.pm4, &data.pm10,
        &data.nc0p5, &data.nc1p0, &data.nc2p5, &data.nc4p0,
        &data.nc10p0, &data.typicalParticleSize);

    if (rc == 0) {
        m_errorCount = 0;
        return true;
    }

    ++m_errorCount;
    Serial.printf("Sensor read failed (#%d): %s\n",
                  m_errorCount,
                  shdlcErrorToString(rc).c_str());
    ErrorHandler::ErrorManager::log(ErrorHandler::ErrorCode::SensorReadFailed,
                                    "Sensor read failed");
    return false;
}

SensorState SensorManager::state() const {
    return m_state;
}

bool SensorManager::isOk() const {
    return m_state == SensorState::Measuring;
}

int SensorManager::errorCount() const {
    return m_errorCount;
}

bool SensorManager::stopMeasurement() {
    bool success = true;

    if (m_state == SensorState::Measuring) {
        const int16_t rc = sps30_stop_measurement();
        if (rc != 0) {
            Serial.printf("Failed to stop measurement: %s\n",
                          shdlcErrorToString(rc).c_str());
            success = false;
        }
    }

    sensirion_uart_hal_free();
    m_state = success ? SensorState::Ready : SensorState::Error;
    return success;
}

void SensorManager::resetErrorCount() {
    m_errorCount = 0;
}

String SensorManager::shdlcErrorToString(int16_t rc) const {
    switch (rc) {
        case 0:
            return "OK";
        case SENSIRION_SHDLC_ERR_NO_DATA:
            return "No data";
        case SENSIRION_SHDLC_ERR_MISSING_START:
            return "Missing start byte";
        case SENSIRION_SHDLC_ERR_MISSING_STOP:
            return "Missing stop byte";
        case SENSIRION_SHDLC_ERR_CRC_MISMATCH:
            return "CRC mismatch";
        case SENSIRION_SHDLC_ERR_ENCODING_ERROR:
            return "Encoding error";
        default: {
            char buffer[24];
            snprintf(buffer, sizeof(buffer), "Error %d", rc);
            return String(buffer);
        }
    }
}

}  // namespace Sensor
