#include <Arduino.h>
#include <Wire.h>

#include "RAK3401Board.h"
#include <nrf_soc.h>

#ifdef NRF52_POWER_MANAGEMENT
// Static configuration for power management
// Values set in variant.h defines
const PowerMgtConfig power_config = {
  .lpcomp_ain_channel = PWRMGT_LPCOMP_AIN,
  .lpcomp_refsel = PWRMGT_LPCOMP_REFSEL,
  .voltage_bootlock = PWRMGT_VOLTAGE_BOOTLOCK
};

static_assert(PWRMGT_VOLTAGE_RESUME > PWRMGT_VOLTAGE_CUTOFF,
              "PWRMGT_VOLTAGE_RESUME must be above PWRMGT_VOLTAGE_CUTOFF (hysteresis)");
static_assert(PWRMGT_VOLTAGE_BOOTLOCK <= PWRMGT_VOLTAGE_RESUME,
              "PWRMGT_VOLTAGE_BOOTLOCK must not exceed PWRMGT_VOLTAGE_RESUME");

void RAK3401Board::initiateShutdown(uint8_t reason) {
  if (reason == SHUTDOWN_REASON_LOW_VOLTAGE ||
      reason == SHUTDOWN_REASON_BOOT_PROTECT) {
    lowVoltageHibernate(reason);  // does not return
  }

  // Disable SKY66122 FEM (CSD+CPS LOW = shutdown, <1 uA)
  digitalWrite(SX126X_POWER_EN, LOW);

  // Disable 3V3 switched peripherals and 5V boost
  digitalWrite(PIN_3V3_EN, LOW);

  enterSystemOff(reason);
}

// Low-voltage hibernate: everything power-hungry off, MCU sleeping, battery
// re-checked periodically. Reboots once VBAT >= PWRMGT_VOLTAGE_RESUME (or USB
// power appears), so the battery gets a real chance to recover via solar
// instead of flickering the node on and off.
void RAK3401Board::lowVoltageHibernate(uint8_t reason) {
  MESH_DEBUG_PRINTLN("PWRMGT: Low voltage (%s) - hibernating until %u mV",
                     getShutdownReasonString(reason), PWRMGT_VOLTAGE_RESUME);

  // At runtime the radio/display/GPS are live; put them to sleep properly.
  // At boot (BOOT_PROTECT) they haven't been initialised yet, so skip that.
  if (reason == SHUTDOWN_REASON_LOW_VOLTAGE) {
    shutdownPeripherals();
  }

  // Disable SKY66122 FEM (CSD+CPS LOW = shutdown, <1 uA)
  digitalWrite(SX126X_POWER_EN, LOW);
  // Disable 3V3_S switched peripherals and the RAK13302 5V boost
  digitalWrite(PIN_3V3_EN, LOW);

  // Record why we went down so `get pwrmgt.bootreason` shows it after resume
  uint8_t sd_enabled = 0;
  sd_softdevice_is_enabled(&sd_enabled);
  if (sd_enabled) {
    sd_power_gpregret_clr(1, 0xFF);
    sd_power_gpregret_set(1, reason);
  } else {
    NRF_POWER->GPREGRET2 = reason;
  }

  for (;;) {
    // FreeRTOS tickless idle: the MCU sleeps (System ON, RTC running) here
    delay((uint32_t)PWRMGT_HIBERNATE_CHECK_SECS * 1000UL);

    if (isExternalPowered()) break;  // USB plugged in: come back immediately

    uint16_t mv = getBattMilliVolts();
    MESH_DEBUG_PRINTLN("PWRMGT: Hibernate check %u mV (resume at %u mV)", mv, PWRMGT_VOLTAGE_RESUME);
    if (mv >= PWRMGT_VOLTAGE_RESUME) break;
  }

  // Clean restart; boot check will pass since VBAT >= RESUME >= BOOTLOCK
  NVIC_SystemReset();
}

// Runtime monitor, called from the main loop via board.loop()
void RAK3401Board::loop() {
  if (PWRMGT_VOLTAGE_CUTOFF == 0) return;  // runtime cutoff disabled
  if ((int32_t)(millis() - lowvolt_next_check) < 0) return;
  lowvolt_next_check = millis() + (uint32_t)PWRMGT_CHECK_INTERVAL_SECS * 1000UL;

  if (isExternalPowered()) {
    lowvolt_count = 0;
    return;
  }

  uint16_t mv = getBattMilliVolts();
  // Ignore obviously bogus readings (<1V) - same guard as the boot check
  if (mv > 1000 && mv < PWRMGT_VOLTAGE_CUTOFF) {
    lowvolt_count++;
    MESH_DEBUG_PRINTLN("PWRMGT: Low battery %u mV (%u/%u)", mv, lowvolt_count, PWRMGT_CUTOFF_CONSECUTIVE);
    if (lowvolt_count >= PWRMGT_CUTOFF_CONSECUTIVE) {
      initiateShutdown(SHUTDOWN_REASON_LOW_VOLTAGE);
    }
  } else {
    lowvolt_count = 0;
  }
}
#endif

void RAK3401Board::begin() {
  NRF52BoardDCDC::begin();
  pinMode(PIN_VBAT_READ, INPUT);
#ifdef PIN_USER_BTN
  pinMode(PIN_USER_BTN, INPUT_PULLUP);
#endif

#ifdef PIN_USER_BTN_ANA
  pinMode(PIN_USER_BTN_ANA, INPUT_PULLUP);
#endif

#if defined(PIN_BOARD_SDA) && defined(PIN_BOARD_SCL)
  Wire.setPins(PIN_BOARD_SDA, PIN_BOARD_SCL);
#endif

  Wire.begin();

  // PIN_3V3_EN (WB_IO2, P0.34) controls the 3V3_S switched peripheral rail
  // AND the 5V boost regulator (U5) on the RAK13302 that powers the SKY66122 PA.
  // Must stay HIGH during radio operation — do not toggle for power saving.
  pinMode(PIN_3V3_EN, OUTPUT);
  digitalWrite(PIN_3V3_EN, HIGH);

  // Enable SKY66122-11 FEM on the RAK13302 module.
  // CSD and CPS are tied together on the RAK13302 PCB, routed to IO3 (P0.21).
  // HIGH = FEM active (LNA for RX, PA path available for TX).
  // TX/RX switching (CTX) is handled by SX1262 DIO2 via SetDIO2AsRfSwitchCtrl.
  pinMode(SX126X_POWER_EN, OUTPUT);
#ifdef NRF52_POWER_MANAGEMENT
  // Boot voltage protection check (may not return if voltage too low)
  // We need to call this after we configure SX126X_POWER_EN as output but before we pull high
  checkBootVoltage(&power_config);
#endif
  digitalWrite(SX126X_POWER_EN, HIGH);
  delay(1);  // SKY66122 turn-on settling time (tON = 3us typ)
}
