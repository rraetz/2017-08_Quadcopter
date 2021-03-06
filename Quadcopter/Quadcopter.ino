#include <I2C.h>  // http://dsscircuits.com/articles/86-articles/66-arduino-i2c-master-library
#include <SPI.h>  // standard Arduino DPI library
#include <RF24.h> // https://github.com/nRF24/RF24

#include "Parameters.h"
#include "MathsHelper.h"
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

// MODE
enum Mode {RATE = 0, ATTITUDE = 1, ATTITUDE_RATEYAW = 3};
Mode mode = RATE;
Mode previousMode = RATE;
bool autoLevel = false;
bool kill = 0;

// STATE
enum State {NOT_ARMED, ARMED, ON_GROUND, TAKING_OFF, FLYING, LANDING, DISABLED, CRASHED, UNSPEC_ERROR};
State state = NOT_ARMED;
State previousState = NOT_ARMED;

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
  state = NOT_ARMED;
  Serial.begin(115200);
  pinMode(pinStatusLed, OUTPUT);
  digitalWrite(pinStatusLed, HIGH);
  setupBatteryMonitor();
  setupI2C();
  setupMotionSensor();
  setupMag();
  setupRadio();
  setupPid();
  // ARMING PROCEDURE
  // wait for radio connection and specific user input (stick up, stick down)
  while (!checkRadioForInput()) {
  }
  while (rcPackage.throttle < 200) {
    checkRadioForInput();
  }
  initialiseCurrentAngles();  // so that hands aren't still fiddling with the battery connection while the gyros are calibrating
  while (rcPackage.throttle > 50) {
    checkRadioForInput();
  }
  // END ARMING
  setupMotors();
  state = ARMED;
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
  state = ON_GROUND;
  digitalWrite(pinStatusLed, LOW);
  //  Serial.println(F("Setup complete"));

} // END SETUP



void loop() {
  loopCounter ++;
  if (millis() - receiverLast >= receiverFreq) {
    receiverLast += receiverFreq;
    receiveAndProcessControlData();
    receiverLoopCounter++;
  }

  manageModeChanges();
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
  // If connection lost then modify throttle so that QC is descending slowly
  if (!rxHeartbeat) {
    calculateVerticalAccel();
    connectionLostDescend(&throttle, valAcZ);
  }
  // don't want to run PIDs if not doing anything to prevent integral building up
  if (state == FLYING) {
    if (autoLevel) {
      setAutoLevelTargets();
    }
    if (mode != RATE) { // i.e. one of the ATTITUDE modes
      setAttitudePidActual(currentAngles.roll, currentAngles.pitch, currentAngles.yaw);
      pidAttitudeUpdate();
      setRatePidTargets(attitudeRollSettings.output, attitudePitchSettings.output, attitudeYawSettings.output);
      if (mode == ATTITUDE_RATEYAW) {
        overrideYawTarget();  // OVERIDE THE YAW ATTITUDE PID OUTPUT with controller output i.e. user controls yaw rate
      }
    }
    setRatePidActual(valGyX, valGyY, valGyZ);
    pidRateUpdate();
  }
}

void receiveAndProcessControlData() {
  checkHeartbeat();  // must be done outside if(radio.available) loop
  if (!rxHeartbeat) {
    autoLevel = true;
    mode = ATTITUDE;  // in the future there may be other scenarios that put the QC into autolevel mode
    if (throttle < THROTTLE_MIN_SPIN) {
      setMotorsLow();
      digitalWrite(pinStatusLed, HIGH);
      state = DISABLED;
      while (1);
    }
    else {
      autoLevel = false;  // this does not come from the controller anymore so needs to be re set even when comms resume
    }
  }
  if (checkRadioForInput()) {
    mode = getMode();
    // MAP CONTROL VALUES
    mapThrottle(&throttle);
    if (mode != RATE) { // i.e. one of the ATTITUDE modes
      mapRcToPidInput(&attitudeRollSettings.target, &attitudePitchSettings.target, &attitudeYawSettings.target, mode);
      // yaw rate target will be overiden in setTargetsAndRunPIDs function for ATTITUDE_RATEYAW mode
    }
    else {  // RATE mode
      mapRcToPidInput(&rateRollSettings.target, &ratePitchSettings.target, &rateYawSettings.target, mode);
    }
  }
}

void manageModeChanges() {
  if (mode != previousMode) {
    previousMode = mode;
    if (mode != RATE) {
      pidAttitudeModeOn();
    }
    else {
      pidAttitudeModeOff();
    }
  }
}

// this doesn't really do much yet but it will influence future behaviour
// also currently very simplistic in how it decides that it is 'flying'
void manageStateChanges() {
  if ( throttle > THROTTLE_MIN_SPIN) {
    state = FLYING;
  }
  else {
    state = ON_GROUND;
  }
}

