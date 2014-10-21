/*
 * power_manager.h
 *
 *  Copyright (c) 2014 Christian Isaksson
 */

#ifndef POWERMANAGER_H_
#define POWERMANAGER_H_

/* I/O pins */

#define RPI_PIN PB0			// Input, Detects if RPi has shutdown
#define PWRSW_PIN PB1		// Input, Detects if power switch is ON or OFF
#define PWRLED_PIN PB2		// Output, power indicator LED
#define MOSFET_PIN PB3		// Output, Gate control to Power supply MOSFET
#define SHUTDOWN_PIN PB4	// Output, signals the Raspberry to shutdown

/* Delays */
#define HARD_POWER_OFF_DELAY 200	// Time power button will need to be pressed until a hard power off is performed
#define SHUTDOWN_DELAY 4000			// Time until power off RPi after RPI_PIN gone low

/* Values */
#define PRESSED_DEBOUNCE_SAMPLES 10
#define RELEASED_DEBOUNCE_SAMPLES 10

/* Type definitions */
typedef enum {
	false = 0,
	true
} boolean;

typedef enum {
	off = 0,
	on,
	shutdown,
	poweroff
} device;

typedef enum {
	released = 0,
	maybe_pressed,
	pressed
} button;

typedef enum {
	low = 0,
	high,
	undefined
} signal;

/* Global Variables */
volatile device raspberryPi;
volatile unsigned int powerButtonCounter;
volatile unsigned long timerOverflowCounter;
volatile button powerButton;
volatile unsigned int tick;

/* Function Prototypes */
void power(boolean);
boolean waitUntilPowerButtonReleased(boolean);


#endif /* POWERMANAGER_H_ */
