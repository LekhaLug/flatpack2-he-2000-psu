#include <Wire.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "driver/twai.h"

// Піни згідно зі схемою підключення
const gpio_num_t CAN_TX_PIN = GPIO_NUM_7;
const gpio_num_t CAN_RX_PIN = GPIO_NUM_10;

const int I2C_SDA_PIN = 5;
const int I2C_SCL_PIN = 6;

const int ENC_A_PIN = 0; // TRA
const int ENC_B_PIN = 1; // TRB
const int BTN_BAK   = 2; // BAK (strapping pin — не утримувати при старті)
const int BTN_PSH   = 3; // PSH
const int BTN_CON   = 4; // CON

// Межі та безпечні значення
const float V_MIN  = 44.5f;
const float V_MAX  = 57.6f;
const float I_MIN  = 0.1f;
const float I_MAX  = 37.4f;
const float V_SAFE = 48.0f;
const float I_SAFE = 5.0f;

const uint32_t CONFIRM_TIMEOUT_MS = 5000UL; // час очікування підтвердження до відкату
const uint32_t BLINK_PERIOD_MS    = 250UL;  // період миготіння непідтвердженого значення

// Діагностика CAN у Serial Monitor
const bool CAN_LOG_RAW = true;
const uint32_t CAN_DIAG_PERIOD_MS = 2000UL;
uint32_t last_can_diag_ms = 0;

Preferences prefs;
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

float set_voltage = V_SAFE;
float set_current = I_SAFE;
float last_saved_voltage = V_SAFE;
float last_saved_current = I_SAFE;

// Стан підтвердження змін
bool pending_confirm = false;
bool pending_is_current = false; // false -> змінюється V, true -> змінюється A
float pending_voltage = V_SAFE;
float pending_current = I_SAFE;
uint32_t pending_start_ms = 0;

bool edit_current = false; // false -> V, true -> A
bool fine_step    = true;  // true -> 0.1, false -> 1.0

uint8_t serial_number[6] = {0};
bool serial_ok = false;
bool current_limiting = false;
int temp_inlet = 0;
int temp_outlet = 0;
int voltage_input = 0;
float voltage_output = 0.0f;
float current_output = 0.0f;
uint32_t last_rx_ms = 0;

volatile int16_t raw_steps = 0;
uint8_t prev_ab = 0;
int16_t detent_accum = 0;

float clampAndRound1(float val, float min_v, float max_v) {
  if (val < min_v) val = min_v;
  if (val > max_v) val = max_v;
  return roundf(val * 10.0f) / 10.0f;
}

void saveSettingsNow() {
  if (fabsf(set_voltage - last_saved_voltage) < 0.01f &&
      fabsf(set_current - last_saved_current) < 0.01f) {
    return;
  }

  prefs.putFloat("v_set", set_voltage);
  prefs.putFloat("i_set", set_current);
  last_saved_voltage = set_voltage;
  last_saved_current = set_current;
}

void beginPendingChange(float new_v, float new_i, bool is_current) {
  pending_voltage = clampAndRound1(new_v, V_MIN, V_MAX);
  pending_current = clampAndRound1(new_i, I_MIN, I_MAX);
  pending_is_current = is_current;
  pending_confirm = true;
  pending_start_ms = millis();
}

void confirmPendingChange() {
  if (!pending_confirm) return;
  set_voltage = pending_voltage;
  set_current = pending_current;
  pending_confirm = false;
  saveSettingsNow();
}

void cancelPendingChange() {
  if (!pending_confirm) return;
  pending_voltage = set_voltage;
  pending_current = set_current;
  pending_confirm = false;
}

void servicePendingTimeout() {
  if (!pending_confirm) return;
  if ((millis() - pending_start_ms) >= CONFIRM_TIMEOUT_MS) {
    cancelPendingChange();
  }
}

void loadSettings() {
  prefs.begin("flatpack2", false);
  set_voltage = clampAndRound1(prefs.getFloat("v_set", V_SAFE), V_MIN, V_MAX);
  set_current = clampAndRound1(prefs.getFloat("i_set", I_SAFE), I_MIN, I_MAX);
  pending_voltage = set_voltage;
  pending_current = set_current;
  last_saved_voltage = set_voltage;
  last_saved_current = set_current;
}

void IRAM_ATTR readEncoderISR() {
  static const int8_t qdec_table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
  };
  uint8_t ab = (digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN);
  uint8_t idx = (prev_ab << 2) | ab;
  raw_steps += qdec_table[idx];
  prev_ab = ab;
}

void applyStep(int dir) {
  float st = fine_step ? 0.1f : 1.0f;
  float base_v = pending_confirm ? pending_voltage : set_voltage;
  float base_i = pending_confirm ? pending_current : set_current;

  if (edit_current) {
    float next_i = clampAndRound1(base_i + dir * st, I_MIN, I_MAX);
    beginPendingChange(base_v, next_i, true);
  } else {
    float next_v = clampAndRound1(base_v + dir * st, V_MIN, V_MAX);
    beginPendingChange(next_v, base_i, false);
  }
}

void handleEncoder() {
  int16_t delta = 0;
  noInterrupts();
  delta = raw_steps;
  raw_steps = 0;
  interrupts();

  detent_accum += delta;
  // За потреби змініть поріг 4 на 2 під ваш енкодер
  while (detent_accum >= 4) {
    applyStep(1);
    detent_accum -= 4;
  }
  while (detent_accum <= -4) {
    applyStep(-1);
    detent_accum += 4;
  }
}

void handleButtons() {
  const uint32_t debounce_ms = 35;
  const uint32_t bak_hold_ms = 600;
  uint32_t now = millis();

  // PSH: підтвердження зміни (якщо очікує підтвердження) або перемикання V <-> A
  static bool psh_prev = HIGH;
  static uint32_t psh_t = 0;
  bool psh_now = digitalRead(BTN_PSH);
  if (psh_now != psh_prev && (now - psh_t) > debounce_ms) {
    psh_t = now;
    psh_prev = psh_now;
    if (psh_now == LOW) {
      if (pending_confirm) {
        confirmPendingChange();
      } else {
        edit_current = !edit_current;
      }
    }
  }

  // CON: перемикання кроку 0.1 <-> 1.0
  static bool con_prev = HIGH;
  static uint32_t con_t = 0;
  bool con_now = digitalRead(BTN_CON);
  if (con_now != con_prev && (now - con_t) > debounce_ms) {
    con_t = now;
    con_prev = con_now;
    if (con_now == LOW) {
      fine_step = !fine_step;
    }
  }

  // BAK: безпечні уставки 48.0V / 5.0A при утриманні 600 мс (потребують підтвердження PSH)
  static bool bak_prev = HIGH;
  static uint32_t bak_down_t = 0;
  static bool bak_fired = false;
  bool bak_now = digitalRead(BTN_BAK);
  if (bak_now == LOW) {
    if (bak_prev == HIGH) {
      bak_down_t = now;
      bak_fired = false;
    } else if (!bak_fired && (now - bak_down_t) >= bak_hold_ms) {
      bak_fired = true;
      edit_current = false;
      beginPendingChange(V_SAFE, I_SAFE, false);
    }
  }
  bak_prev = bak_now;
}

void logCanFrame(const char* dir, uint32_t ext_id, const uint8_t* data, uint8_t len) {
  if (!CAN_LOG_RAW) return;
  Serial.printf("[CAN %s] ID=0x%08lX DLC=%u DATA=", dir, (unsigned long)ext_id, len);
  for (uint8_t i = 0; i < len && i < 8; i++) {
    Serial.printf("%02X%s", data[i], (i < len - 1 && i < 7) ? " " : "");
  }
  Serial.println();
}

void logCanDiagnostics() {
  uint32_t now = millis();
  if ((now - last_can_diag_ms) < CAN_DIAG_PERIOD_MS) return;
  last_can_diag_ms = now;

  twai_status_info_t status;
  esp_err_t st_err = twai_get_status_info(&status);

  bool link = (last_rx_ms != 0) && ((now - last_rx_ms) < 3000UL);
  Serial.printf("[CAN DIAG] link=%s serial_ok=%s age_rx=%lu ms ",
                link ? "OK" : "NO_LINK",
                serial_ok ? "YES" : "NO",
                last_rx_ms ? (unsigned long)(now - last_rx_ms) : 0UL);

  if (serial_ok) {
    Serial.printf("SN=%02X%02X%02X%02X%02X%02X ",
                  serial_number[0], serial_number[1], serial_number[2],
                  serial_number[3], serial_number[4], serial_number[5]);
  }

  if (st_err == ESP_OK) {
    Serial.printf("state=%d tx_err=%lu rx_err=%lu rx_miss=%lu arb_lost=%lu bus_err=%lu",
                  (int)status.state,
                  (unsigned long)status.tx_error_counter,
                  (unsigned long)status.rx_error_counter,
                  (unsigned long)status.rx_missed_count,
                  (unsigned long)status.arb_lost_count,
                  (unsigned long)status.bus_error_count);
  }
  Serial.println();
}

bool initCanTwai() {
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
  g_config.rx_queue_len = 32;
  g_config.tx_queue_len = 16;

  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_125KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
  if (err != ESP_OK) {
    Serial.printf("[CAN ERR] twai_driver_install failed: %d\n", (int)err);
    return false;
  }
  err = twai_start();
  if (err != ESP_OK) {
    Serial.printf("[CAN ERR] twai_start failed: %d\n", (int)err);
    return false;
  }
  Serial.println("[CAN OK] TWAI started at 125 kbps (TX=GPIO7, RX=GPIO10)");
  return true;
}

void processCanRx() {
  twai_message_t msg;
  while (twai_receive(&msg, 0) == ESP_OK) {
    if (!msg.extd) continue; // потрібен Extended ID
    uint32_t ext_id = msg.identifier & 0x1FFFFFFFUL;
    const uint8_t* buf = msg.data;
    uint8_t len = msg.data_length_code;

    logCanFrame("RX", ext_id, buf, len);

    // Welcome-кадр: маска 0x1FFF0000 на 0x05000000, серійник у байтах 1..6
    if ((ext_id & 0x1FFF0000UL) == 0x05000000UL && len >= 7) {
      bool first_time = !serial_ok;
      for (int i = 0; i < 6; i++) serial_number[i] = buf[i + 1];
      serial_ok = true;
      if (first_time) {
        Serial.printf("[CAN INFO] Welcome SN: %02X%02X%02X%02X%02X%02X\n",
                      serial_number[0], serial_number[1], serial_number[2],
                      serial_number[3], serial_number[4], serial_number[5]);
      }
    }
    // Статус модуля (0x05014004 — CV, 0x05014008 — CC)
    else if ((ext_id == 0x05014004UL || ext_id == 0x05014008UL) && len >= 8) {
      current_limiting = (ext_id == 0x05014008UL);
      temp_inlet     = buf[0];
      current_output = (256 * buf[2] + buf[1]) / 10.0f;
      if (current_output > 100.0f) current_output = 0.0f;
      voltage_output = (256 * buf[4] + buf[3]) / 100.0f;
      voltage_input  = 256 * buf[6] + buf[5];
      temp_outlet    = buf[7];
      last_rx_ms     = millis();
    }
  }
}

void sendCanFrame(uint32_t ext_id, const uint8_t data[8]) {
  twai_message_t msg = {};
  msg.identifier = ext_id;
  msg.extd = 1;
  msg.rtr = 0;
  msg.data_length_code = 8;
  memcpy(msg.data, data, 8);

  esp_err_t res = twai_transmit(&msg, pdMS_TO_TICKS(10));
  if (res == ESP_OK) {
    logCanFrame("TX", ext_id, data, 8);
  } else {
    Serial.printf("[CAN TX ERR] ID=0x%08lX err=%d\n", (unsigned long)ext_id, (int)res);
  }
}

void sendCanTx() {
  if (!serial_ok) {
    if (CAN_LOG_RAW) {
      Serial.println("[CAN TX SKIP] Waiting for welcome frame (0x0500xxxx)");
    }
    return;
  }

  uint8_t login_data[8] = {
    serial_number[0], serial_number[1], serial_number[2],
    serial_number[3], serial_number[4], serial_number[5],
    0x00, 0x00
  };
  sendCanFrame(0x05004804UL, login_data);

  uint16_t i10  = (uint16_t)lroundf(set_current * 10.0f);
  uint16_t u100 = (uint16_t)lroundf(set_voltage * 100.0f);
  uint8_t set_data[8] = {
    (uint8_t)(i10 & 0xFF),  (uint8_t)(i10 >> 8),
    (uint8_t)(u100 & 0xFF), (uint8_t)(u100 >> 8),
    (uint8_t)(u100 & 0xFF), (uint8_t)(u100 >> 8),
    0x3E, 0x17
  };
  sendCanFrame(0x05FF4004UL, set_data);
}

void drawDisplay() {
  uint32_t now = millis();
  bool link = (last_rx_ms != 0) && ((now - last_rx_ms) < 3000UL);
  bool blink_on = ((now / BLINK_PERIOD_MS) % 2UL) == 0UL;

  float disp_v = pending_confirm ? pending_voltage : set_voltage;
  float disp_i = pending_confirm ? pending_current : set_current;

  bool v_blink_hide = pending_confirm && !pending_is_current && !blink_on;
  bool i_blink_hide = pending_confirm && pending_is_current && !blink_on;

  char buf_v[10];
  char buf_a[10];
  char line[36];

  dtostrf(disp_v, 4, 1, buf_v);
  dtostrf(disp_i, 4, 1, buf_a);

  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  // Верхній ряд уставок: активний параметр виділений інверсією, цифри на фіксованих місцях
  u8g2.setFont(u8g2_font_profont17_mr);

  // Напруга (V)
  snprintf(line, sizeof(line), "%sV", buf_v);
  if (!edit_current) {
    u8g2.drawBox(0, 2, 56, 17);
    if (!v_blink_hide) {
      u8g2.setDrawColor(0);
      u8g2.drawStr(2, 17, line);
      u8g2.setDrawColor(1);
    }
  } else if (!v_blink_hide) {
    u8g2.drawStr(2, 17, line);
  }

  // Струм (A)
  snprintf(line, sizeof(line), "%sA", buf_a);
  if (edit_current) {
    u8g2.drawBox(66, 2, 56, 17);
    if (!i_blink_hide) {
      u8g2.setDrawColor(0);
      u8g2.drawStr(68, 17, line);
      u8g2.setDrawColor(1);
    }
  } else if (!i_blink_hide) {
    u8g2.drawStr(68, 17, line);
  }

  // Роздільна лінія
  u8g2.drawHLine(0, 24, 128);

  // Нижня зона
  u8g2.setFont(u8g2_font_6x10_mr);

  // Правий нижній блок: лише крок
  snprintf(line, sizeof(line), "S:%s", fine_step ? "0.1" : "1.0");
  u8g2.drawStr(86, 42, line);

  // Лівий нижній блок: стан CAN або телеметрія
  if (!link) {
    u8g2.drawStr(0, 48, "NO LINK");
  } else {
    snprintf(line, sizeof(line), "%s IN:%3dV",
             current_limiting ? "CC" : "CV",
             voltage_input);
    u8g2.drawStr(0, 42, line);

    snprintf(line, sizeof(line), "%.1fV %.1fA",
             voltage_output, current_output);
    u8g2.drawStr(0, 56, line);
  }

  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  uint32_t s_start = millis();
  while (!Serial && (millis() - s_start < 1500UL)) {
    delay(10);
  }
  Serial.println("\n[BOOT] Flatpack2 ESP32-C3 controller starting...");

  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  pinMode(BTN_PSH, INPUT_PULLUP);
  pinMode(BTN_CON, INPUT_PULLUP);
  pinMode(BTN_BAK, INPUT_PULLUP);

  loadSettings();
  Serial.printf("[CFG] Loaded V=%.1fV, I=%.1fA\n", set_voltage, set_current);

  prev_ab = (digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN);
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), readEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), readEncoderISR, CHANGE);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 200000);
  u8g2.begin();

  initCanTwai();
}

void loop() {
  handleEncoder();
  handleButtons();
  servicePendingTimeout();
  processCanRx();
  logCanDiagnostics();

  static uint32_t last_tx = 0;
  if (millis() - last_tx >= 1000UL) {
    last_tx = millis();
    sendCanTx();
  }

  static uint32_t last_disp = 0;
  if (millis() - last_disp >= 150UL) {
    last_disp = millis();
    drawDisplay();
  }
}