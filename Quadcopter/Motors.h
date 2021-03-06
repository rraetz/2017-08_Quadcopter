const uint16_t CYCLE_TICKS = 5000; // 10000ticks, 5000us, 5ms, 200Hz
uint16_t escTicks[4];
uint16_t escTicksEndMain[4];
uint16_t volatile escTicksEndTemp[4];
uint16_t escTicksEndIsr[4];  // copy to use in IRS
uint8_t escOrderMain[4];
uint8_t volatile escOrderTemp[4];
uint8_t escOrderIsr[4];  // copy to use in IRS
uint8_t escIndex = 0;
bool volatile lockPulses = false;
bool volatile needUpdatePulses = false; // this needs to be set to true after the rate PID runs
enum GenerationCycle {START_PULSES, END_PULSES, RESET};
GenerationCycle escPulseGenerationCycle = RESET;
const uint16_t PULSE_GAP = 100;  // gap between starting pulses, in ticks
const uint16_t escTicksStart[4] = {PULSE_GAP, PULSE_GAP * 2, PULSE_GAP * 3, PULSE_GAP * 4};

// these pins are currently hardcoded within the pulse generation function (direct port manipulation)
const byte pinMotor1 = 3; // front left (CW)
const byte pinMotor2 = 6; // front right (CCW)
const byte pinMotor3 = 4; // back left (CCW)
const byte pinMotor4 = 5; // back right (CW)

int motor1pulse;
int motor2pulse;
int motor3pulse;
int motor4pulse;

// ****************************************************************************************
//        FUNCTIONS FOR CALCULATING MOTOR PULSES
// ****************************************************************************************

void calculateMotorInput(int throttle, float rollOffset, float pitchOffset, float yawOffset) {
  motor1pulse = throttle + (int) rollOffset - (int) pitchOffset + (int) yawOffset;
  motor2pulse = throttle - (int) rollOffset - (int) pitchOffset - (int) yawOffset;
  motor3pulse = throttle + (int) rollOffset + (int) pitchOffset - (int) yawOffset;
  motor4pulse = throttle - (int) rollOffset + (int) pitchOffset + (int) yawOffset;
}

void capMotorInputNearMaxThrottle() {
  int maxMotorValue = max(motor1pulse, max(motor2pulse, max(motor3pulse, motor4pulse)));
  int adj = maxMotorValue - THROTTLE_LIMIT;
  if (adj > 0) {
    motor1pulse -= adj;
    motor2pulse -= adj;
    motor3pulse -= adj;
    motor4pulse -= adj;
  }
}

void capMotorInputNearMinThrottle(int throttle) {
  if (throttle < THROTTLE_MIN_SPIN) { //this is the base throttle i.e. if stick is low, no motor should move even if there's a big movements
    motor1pulse = ZERO_THROTTLE;
    motor2pulse = ZERO_THROTTLE;
    motor3pulse = ZERO_THROTTLE;
    motor4pulse = ZERO_THROTTLE;
  }
  else {
    int minMotorValue = min(motor1pulse, min(motor2pulse, min(motor3pulse, motor4pulse)));
    int adj = THROTTLE_MIN_SPIN - minMotorValue;
    if (adj > 0) {
      motor1pulse += adj;
      motor2pulse += adj;
      motor3pulse += adj;
      motor4pulse += adj;
    }
  }
}



// ****************************************************************************************
//        FUNCTIONS FOR ESC CREATION
// ****************************************************************************************

// configure timer1 for generating pulses for the ESCs
static void setupPulseTimer() {
  cli();
  TCCR1A = 0;             // normal counting mode
  TCCR1B = _BV(CS11);     // set prescaler of 8 - 2 ticks per microsecond
  TCNT1 = 0;              // clear the timer count
  TIFR1 |= _BV(OCF1A);     // clear any pending interrupts;
  TIMSK1 |=  _BV(OCIE1A) ; // enable the output compare interrupt
  OCR1A = CYCLE_TICKS;
  sei(); // enable interrupts
}

static void endPulseTimer() {
  cli();
  TIMSK1 =  0 ; // disable the output compare interrupt
  TIFR1 |= _BV(OCF1A);     // clear any pending interrupts;
  sei();
}

static inline void generate_esc_pulses() {

  if (escPulseGenerationCycle == START_PULSES) {  // interupt has fired and we're starting the pulses
    uint8_t currentEsc = escOrderIsr[escIndex];
    if (currentEsc == 1) PORTD |= B00001000; // set bit/pin 3 HIGH
    if (currentEsc == 2) PORTD |= B01000000; // set bit/pin 6 HIGH
    if (currentEsc == 3) PORTD |= B00010000; // set bit/pin 4 HIGH
    if (currentEsc == 4) PORTD |= B00100000; // set bit/pin 5 HIGH
    escIndex++;
    if (escIndex > 3) {
      escIndex = 0;
      OCR1A = escTicksEndIsr[0]; // set to end time of first pulse
      escPulseGenerationCycle = END_PULSES;
    }
    else {
      OCR1A = escTicksStart[escIndex];  // next interrupt when the next pulse needs to start
    }
  }

  else if (escPulseGenerationCycle == END_PULSES) {  // interupt has fired and we're ending the pulses
    uint8_t currentEsc = escOrderIsr[escIndex];
    if (currentEsc == 1) PORTD &= B11110111;
    if (currentEsc == 2) PORTD &= B10111111;
    if (currentEsc == 3) PORTD &= B11101111;
    if (currentEsc == 4) PORTD &= B11011111;
    escIndex++;
    if (escIndex > 3) {
      OCR1A = CYCLE_TICKS;
      escIndex = 0;
      escPulseGenerationCycle = RESET; // reset back to beginning
      // new data is available then update the ISR variables
      if (!lockPulses && needUpdatePulses) {
        escOrderIsr[0] = escOrderTemp[0];
        escOrderIsr[1] = escOrderTemp[1];
        escOrderIsr[2] = escOrderTemp[2];
        escOrderIsr[3] = escOrderTemp[3];
        escTicksEndIsr[0] = escTicksEndTemp[0];
        escTicksEndIsr[1] = escTicksEndTemp[1];
        escTicksEndIsr[2] = escTicksEndTemp[2];
        escTicksEndIsr[3] = escTicksEndTemp[3];
        needUpdatePulses = false;
      }
    }
    else {
      OCR1A = escTicksEndIsr[escIndex];
    }
  }

  else {  // i.e. escPulseGenerationCycle = RESET;
    TCNT1 = 0;  // reset the timer
    OCR1A = PULSE_GAP;  // start again after the standard gap
    escPulseGenerationCycle = START_PULSES; // next time interupt fire we want to start the pulses
  }
}

ISR(TIMER1_COMPA_vect) {
  generate_esc_pulses();
}

// note that interupts are expected to be turned off when this is called
void makePulseInfoAvailableToISR() {
  lockPulses = true;
  escOrderTemp[0] = escOrderMain[0];
  escOrderTemp[1] = escOrderMain[1];
  escOrderTemp[2] = escOrderMain[2];
  escOrderTemp[3] = escOrderMain[3];
  escTicksEndTemp[0] = escTicksEndMain[0];
  escTicksEndTemp[1] = escTicksEndMain[1];
  escTicksEndTemp[2] = escTicksEndMain[2];
  escTicksEndTemp[3] = escTicksEndMain[3];
  lockPulses = false;
  needUpdatePulses = true;
}

void calculateRequiredTicks() {
  escTicks[0] = motor1pulse << 1;
  escTicks[1] = motor2pulse << 1;
  escTicks[2] = motor3pulse << 1;
  escTicks[3] = motor4pulse << 1;
}

void resetOrder() {
  escOrderMain[0] = 1;
  escOrderMain[1] = 2;
  escOrderMain[2] = 3;
  escOrderMain[3] = 4;
}

void sortPulses() {
  uint16_t j;
  uint8_t jIdx;
  int16_t k;
  for (uint8_t i = 1; i < 4; ++i)
  {
    j = escTicks[i];
    jIdx = escOrderMain[i];
    k;
    for (k = i - 1; (k >= 0) && (j < escTicks[k]); k--)
    {
      escTicks[k + 1] = escTicks[k];
      escOrderMain[k + 1] = escOrderMain[k];
    }
    escTicks[k + 1] = j;
    escOrderMain[k + 1] = jIdx;
  }
}

void calcEndTimes() {
  escTicksEndMain[0] = escTicks[0] + PULSE_GAP;
  escTicksEndMain[1] = escTicks[1] + (2 * PULSE_GAP);
  escTicksEndMain[2] = escTicks[2] + (3 * PULSE_GAP);
  escTicksEndMain[3] = escTicks[3] + (4 * PULSE_GAP);
}

void recalculateMotorPulses() {
  resetOrder(); // reset escOrderMain
  calculateRequiredTicks(); // populate escTicks
  sortPulses(); // reorder escOrderMain and escTicks
  calcEndTimes(); // what times should these finish - populate escTicksEndMain
  makePulseInfoAvailableToISR();
}

void processMotors(int throttle, float rateRollOutput, float ratePitchOutput, float rateYawOutput) {
  calculateMotorInput(throttle, rateRollOutput, ratePitchOutput, rateYawOutput);
  capMotorInputNearMaxThrottle();
  capMotorInputNearMinThrottle(throttle);
  recalculateMotorPulses();
}

// ****************************************************************************************
//        SETUP FOR MAIN FILE
// ****************************************************************************************

void setupMotors() {
  pinMode(pinMotor1, OUTPUT);
  pinMode(pinMotor2, OUTPUT);
  pinMode(pinMotor3, OUTPUT);
  pinMode(pinMotor4, OUTPUT);
  escTicks[1] = 2000;  // set starting pulse to 0.4micros (out of ESC range)
  escTicks[2] = 2000;
  escTicks[3] = 2000;
  escTicks[4] = 2000;
  setupPulseTimer();
}

// ****************************************************************************************
//        OTHER
// ****************************************************************************************


void setMotorsLow() {
  motor1pulse = 1000;
  motor2pulse = 1000;
  motor3pulse = 1000;
  motor4pulse = 1000;
  recalculateMotorPulses();
}


