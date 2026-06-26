#include "hw/imu.h"
#include "hw/pins.h"
#include <Arduino.h>
#include <Wire.h>

#if BOARD_HAS_QMI8658
#include <SensorQMI8658.hpp>
static SensorQMI8658 s_qmi;
#endif

bool hwImuInit() {
#if !BOARD_HAS_QMI8658
  Serial.println("hwImu: no QMI8658 on this board");
  return true;
#else
  if (!s_qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL)) {
    Serial.println("hwImu: QMI8658 begin failed");
    return false;
  }
  Serial.printf("hwImu: WHO_AM_I=0x%02X chipID=0x%02X\n",
                s_qmi.whoAmI(), s_qmi.getChipID());
  // Reset to a clean default state after warm flash/reset.
  s_qmi.reset();
  s_qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_125Hz);
  s_qmi.enableAccelerometer();
  return true;
#endif
}

void hwImuAccel(float* ax, float* ay, float* az) {
#if !BOARD_HAS_QMI8658
  *ax = 0;
  *ay = 0;
  *az = 0;
#else
  IMUdata d;
  s_qmi.getAccelerometer(d.x, d.y, d.z);
  *ax = d.x;
  *ay = d.y;
  // Screen-up reads negative; the face-down detector expects az < -0.7.
  *az = -d.z;
#endif
}
