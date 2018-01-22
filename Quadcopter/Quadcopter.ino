#include <I2C.h>  // http://dsscircuits.com/articles/86-articles/66-arduino-i2c-master-library
#include <SPI.h>  // standard Arduino DPI library
#include <RF24.h> // https://github.com/nRF24/RF24

#include "Parameters.h"
#include "PID.h"
#include "BatteryMonitor.h"
#include "I2cFunctions.h"
#include "MotionSensor.h"
#include "Receiver.h"
#include "Motors.h"
#include "PIDSettings.h"
#include "DebugPrints.h"

// THROTTLE
int throttle;  // distinct from the user input because it may be modified

// STATE
const bool RATE = false;
const bool ATTITUDE = true;
bool mode = RATE; // MODE IS ONLY FOR RATE or ATTITUDE
bool previousMode = RATE;
bool autoLevel = false;
bool kill = 0;

// CONTROL LOOPS
unsigned long receiverLast = 0;
unsigned long batteryLoopLast = 0;
unsigned long mainLoopLast = 0;
unsigned long gyroLoopLast = 0;
unsigned long magLoopLast = 0;

// DEBUGGING // PERFORMANCE CHECKING
unsigned long lastPrint = 0;
unsigned long functionTimeSum = 0;
uint16_t functionTimeCounter = 0;
unsigned long tStart;
unsigned long tEnd;
uint16_t loopCounter = 0;
uint16_t gyroLoopCounter = 0;
uint16_t receiverLoopCounter = 0;
uint16_t mainLoopCounter = 0;
uint16_t magLoopCounter = 0;


void setup() {
  Serial.begin(115200);
  pinMode(pinStatusLed, OUTPUT);
  digitalWrite(pinStatusLed, HIGH);
  setupBatteryMonitor();
  setupI2C();
  setupMotionSensor();
  setupMag();
  initialiseCurrentAngles();
  setupRadio();
  setupPid();
  // ARMING PROCEDURE
  // wait for radio connection and specific user input (stick up, stick down)
  while (!checkRadioForInput()) {
  }
  while (rcPackage.throttle < 200) {
    checkRadioForInput();
  }
  while (rcPackage.throttle > 50) {
    checkRadioForInput();
  }
  // END ARMING
  setupMotors();
  Serial.println(F("Setup complete"));
  digitalWrite(pinStatusLed, LOW);
  checkHeartbeat(); // refresh
  pidRateModeOn();
  unsigned long startTimeMillis = millis();
  lastPrint = startTimeMillis; // for debug output
  receiverLast = startTimeMillis;
  batteryLoopLast = startTimeMillis;
  magLoopLast = startTimeMillis;
  unsigned long startTimeMicros = micros();
  mainLoopLast = startTimeMicros;
  gyroLoopLast = startTimeMicros;

} // END SETUP



void loop() {
  loopCounter ++;
  if (millis() - receiverLast >= receiverFreq) {
    receiverLast += receiverFreq;
    receiveAndProcessControlData();
    receiverLoopCounter++;
  }

  manageStateChanges();

  if (micros() - gyroLoopLast >= gyroLoopFreq) {
    gyroLoopLast += gyroLoopFreq;
    readGyros();
    processGyroData();
    gyroLoopCounter++;
  }
  if (micros() - mainLoopLast >= mainLoopFreq) {
    mainLoopLast += mainLoopFreq;
    readAccels();
    processAccelData();
    combineGyroAccelData();
    setTargetsAndRunPIDs();
    processMotors(throttle, rateRollSettings.output, ratePitchSettings.output, rateYawSettings.output);
    mainLoopCounter++;

  }
  if (millis() - magLoopLast >= magLoopFreq) {
    magLoopLast += magLoopFreq;
    readMag();
    processMagData();
    combineGyroMagHeadings();
    magLoopCounter++;
  }


  updateMotorPulseISR(); // keep trying to update the actual esc pulses in the ISR in case it was locked previously

  if (millis() - batteryLoopLast >= batteryFreq) {
    batteryLoopLast += batteryFreq;
    calculateBatteryLevel();
  }

  // ****************************************************************************************
  // DEBUGGING
  // ****************************************************************************************

  //  if (millis() - lastPrint >= 1000) {
  //    lastPrint += 1000;
  //    Serial.print(loopCounter); Serial.print('\t');
  //    Serial.print(gyroLoopCounter); Serial.print('\t');
  //    Serial.print(mainLoopCounter); Serial.print('\t');
  //    Serial.print(magLoopCounter); Serial.print('\t');
  //    Serial.print(receiverLoopCounter); Serial.print('\n');
  //    loopCounter = 0;
  //    gyroLoopCounter = 0;
  //    mainLoopCounter = 0;
  //    magLoopCounter = 0;
  //    receiverLoopCounter = 0;
  //  }

} // END LOOP


void setTargetsAndRunPIDs() {
  if (autoLevel) { // if no communication received, OR user has specified auto-level
    setAutoLevelTargets();
    // If connection lost then also modify throttle so that QC is descending slowly
    if (!rxHeartbeat) {
      calculateVerticalAccel();
      connectionLostDescend(&throttle, valAcZ);
    }
  }
  if (mode) {
    setAttitudePidActual(currentAngles.roll, currentAngles.pitch, currentAngles.yaw);
    pidAttitudeUpdate();
    setRatePidTargets(attitudeRollSettings.output, attitudePitchSettings.output, attitudeYawSettings.output);
    //    overrideYawTarget();  // OVERIDE THE YAW ATTITUDE PID OUTPUT
  }
  setRatePidActual(valGyX, valGyY, valGyZ);
  pidRateUpdate();
}

void receiveAndProcessControlData() {
  checkHeartbeat();  // must be done outside if(radio.available) loop
  if (checkRadioForInput()) {
    // CHECK MODES
    mode = getMode();
    autoLevel = getAutolevel();
    if (getKill() && (throttle < 1050)) {
      setMotorsLow();
      digitalWrite(pinStatusLed, HIGH);
      while (1);
    }
    // MAP CONTROL VALUES
    mapThrottle(&throttle);
    if (mode || autoLevel) {
      mapRcToPidInput(&attitudeRollSettings.target, &attitudePitchSettings.target, &attitudeYawSettings.target, mode);
      mode = ATTITUDE;
    }
    else {
      mapRcToPidInput(&rateRollSettings.target, &ratePitchSettings.target, &rateYawSettings.target, mode);
      mode = RATE;
    }
  }
  autoLevel = autoLevel || !rxHeartbeat;
  if (autoLevel) {
    mode = ATTITUDE;
  }
}


void manageStateChanges() {
  if (mode != previousMode) {
    if (mode == ATTITUDE) {
      pidAttitudeModeOn();
      previousMode = ATTITUDE;
    }
    else {
      pidAttitudeModeOff();
      previousMode = RATE;
    }
  }
}

