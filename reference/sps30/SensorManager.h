#pragma once

#include <Arduino.h>
#include <sensirion_shdlc.h>
#include <sensirion_uart_hal.h>
#include <sps30_uart.h>

namespace Sensor {

enum class SensorState {
    Uninitialized,
    Initializing,
    Ready,
    Measuring,
    Error,
};

struct SensorData {
    float pm1;
    float pm2_5;
    float pm4;
    float pm10;
    float nc0p5;
    float nc1p0;
    float nc2p5;
    float nc4p0;
    float nc10p0;
    float typicalParticleSize;

    SensorData()
        : pm1(0.0f),
          pm2_5(0.0f),
          pm4(0.0f),
          pm10(0.0f),
          nc0p5(0.0f),
          nc1p0(0.0f),
          nc2p5(0.0f),
          nc4p0(0.0f),
          nc10p0(0.0f),
          typicalParticleSize(0.0f) {
    }
};

class SensorManager {
public:
    SensorManager();
    ~SensorManager();

    bool begin();
    bool readData(SensorData& data);
    SensorState state() const;
    bool isOk() const;
    int errorCount() const;
    bool stopMeasurement();
    void resetErrorCount();

private:
    String shdlcErrorToString(int16_t rc) const;

    SensorState m_state;
    int m_errorCount;
};

}  // namespace Sensor
