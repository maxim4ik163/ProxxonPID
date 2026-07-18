#include <Arduino.h>

// =========================== CONFIG ===========================
#define MAX_TARGET_EMF 10000  // максимальный целевой BEMF [мВ]
#define DIV_R1 10000          // верхнее плечо делителя [Ом]
#define DIV_R2 2200           // нижнее плечо делителя [Ом]
#define PID_DIVIDER 4000.0    // делитель коэффициентов (выход ацп 0-1023 делится на него)

// system
#define MIN_TARGET_EMF 200  // минимальный целевой BEMF [мВ]
#define REF1_1 1100         // напряжение опорного 1.1V [мВ]
#define MIN_VMOT 6000       // минимальное питание [мВ]
#define CTRL_PRD 20         // период управления [мс]
#define WAIT_ADC 800        // ожидание после отключения ШИМ [мкс]
#define SMOOTH_STEP 100     // изменение целевой BEMF за период [мВ]

// пины
#define BEMF_PIN A0   // пин измерения BEMF
#define VMOT_PIN A1   // пин измерения питания
#define SPEED_PIN A2  // пин скорости
#define P_PIN A3      // пин P
#define I_PIN A4      // пин I

// =========================== DATA ===========================
int vref;        // реф напряжение
int target = 0;  // таргет emf
int pwm = 0;     // текущий шим

// ========================== IntEMA ==========================
template <typename T>
class IntEMA {
   public:
    // k2 - коэффициент как степень двойки
    T filter(T val, uint8_t k2) {
        T sum = (val - _filt) + _err;
        T div = sum >> k2;
        _err = sum - (div << k2);
        return _filt += div;
    }

    void init(T val) {
        _filt = val;
        _err = 0;
    }

    T get() {
        return _filt;
    }

    operator T() {
        return _filt;
    }

   private:
    T _filt = 0, _err = 0;
};

IntEMA<int> vmot;
IntEMA<int> vemf;
IntEMA<int> a_tar;
IntEMA<int> a_p;
IntEMA<int> a_i;

// =========================== PIreg ===========================
// ПИ регулятор
class PIreg {
   public:
    float Kp = 0;
    float Ki = 0;
    float integral = 0;

    int compute(int input, int setpoint, int feedforward, float dt) {
        float error = setpoint - input;
        float nextIntegral = integral + error * Ki * dt;
        float output = feedforward + error * Kp + nextIntegral;

        if (!((output > 255 && error > 0) || (output < 0 && error < 0))) {
            integral = nextIntegral;
        }

        return constrain(output, 0, 255);
    }
};

PIreg pi;

// =========================== FUNC ===========================

// ШИМ 16 кГц "8 Бит" на пине 3
void setPWM(uint8_t duty) {
    TCCR2A = 1 << COM2B1 | 1 << WGM21 | 1 << WGM20;
    TCCR2B = (1 << WGM22) | 0b010;
    OCR2A = (F_CPU / (16000UL * 8)) - 1;
    if (duty == 0) {
        bitClear(TCCR2A, COM2B1);
        digitalWrite(3, LOW);
    } else {
        bitSet(TCCR2A, COM2B1);
        OCR2B = map(duty, 0, 255, 0, OCR2A);
    }
}

// чтение напряжения питания, мв
uint16_t readVCC(uint16_t ref1v1) {
    uint8_t muxt = ADMUX;
    ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
    delay(1);
    ADCSRA |= _BV(ADSC);
    while (bit_is_set(ADCSRA, ADSC));
    uint16_t vcc = ref1v1 * 1023ul / ADC;
    ADMUX = muxt;
    return vcc;
}

// чтение с делителя напряжения, мв
uint16_t readDivider(uint8_t apin) {
    uint32_t pinMv = (uint32_t)analogRead(apin) * vref / 1024;
    return pinMv * (DIV_R1 + DIV_R2) / DIV_R2;
}

// измерение BEFM и управление
void control() {
    static uint32_t tmr;
    if (millis() - tmr < CTRL_PRD) return;
    tmr = millis();

    // измеряем bemf
    setPWM(0);
    delayMicroseconds(WAIT_ADC);

    int rawVmot = readDivider(VMOT_PIN);
    int rawMotor = readDivider(BEMF_PIN);
    int rawBemf = rawVmot - rawMotor;

    // пока считаем математику, возвращаем старый PWM
    if (rawVmot > MIN_VMOT) setPWM(pwm);

    vmot.filter(rawVmot, 2);
    vemf.filter(rawBemf, 2);
    a_tar.filter(1023 - analogRead(SPEED_PIN), 3);
    a_p.filter(1023 - analogRead(P_PIN), 3);
    a_i.filter(1023 - analogRead(I_PIN), 3);

    int newTarget = (long)a_tar.get() * MAX_TARGET_EMF / 1024;

    pi.Kp = a_p.get() / PID_DIVIDER;
    pi.Ki = a_i.get() / PID_DIVIDER;

    if (rawVmot <= MIN_VMOT || newTarget < MIN_TARGET_EMF) {
        pi.integral = 0;
        target = 0;
        pwm = 0;
        setPWM(0);
        return;
    }

    // плавное изменение таргета
    target += constrain(newTarget - target, -SMOOTH_STEP, SMOOTH_STEP);

    // расчётный ШИМ
    int feedforward = (long)target * 255 / vmot.get();

    // ПИД
    pwm = pi.compute(vemf.get(), target, feedforward, CTRL_PRD / 1000.0f);

    setPWM(pwm);
}

// лог
void log() {
    static uint32_t tmr;
    if (millis() - tmr < 100) return;

    tmr = millis();
    Serial.println("#vmot,vemf,tar,pwm10");
    Serial.print('$');
    Serial.print(vmot.get());
    Serial.print(',');
    Serial.print(vemf.get());
    Serial.print(',');
    Serial.print(target);
    Serial.print(',');
    Serial.print(pwm * 10);
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    vref = readVCC(REF1_1);
    pinMode(3, OUTPUT);
}

void loop() {
    control();
    log();
}
