
#include <user_interface.h>
#include <LittleFS.h>

#include "ratgdo.h"
#include "wifi.h"
#include "homekit.h"
#include "comms.h"
#include "log.h"
#include "web.h"
#include "utilities.h"

#include <time.h>
#include <coredecls.h>

time_t now = 0;
tm timeInfo;

void showTime()
{
    localtime_r(&now, &timeInfo); // update the structure tm with the current time
    Serial.print("year:");
    Serial.print(timeInfo.tm_year + 1900); // years since 1900
    Serial.print("\tmonth:");
    Serial.print(timeInfo.tm_mon + 1); // January = 0 (!)
    Serial.print("\tday:");
    Serial.print(timeInfo.tm_mday); // day of month
    Serial.print("\thour:");
    Serial.print(timeInfo.tm_hour); // hours since midnight  0-23
    Serial.print("\tmin:");
    Serial.print(timeInfo.tm_min); // minutes after the hour  0-59
    Serial.print("\tsec:");
    Serial.print(timeInfo.tm_sec); // seconds after the minute  0-61*
    Serial.print("\twday");
    Serial.print(timeInfo.tm_wday); // days since Sunday 0-6
    if (timeInfo.tm_isdst == 1)     // Daylight Saving Time flag
        Serial.print("\tDST");
    else
        Serial.print("\tstandard");
    Serial.println();
}

/********************************* FWD DECLARATIONS *****************************************/

void setup_pins();
void IRAM_ATTR isr_obstruction();
void service_timer_loop();

/********************************* RUNTIME STORAGE *****************************************/

struct obstruction_sensor_t
{
    unsigned int low_count = 0;    // count obstruction low pulses
    unsigned long last_asleep = 0; // count time between high pulses from the obst ISR
} obstruction_sensor;

// long unsigned int led_reset_time = 0; // Stores time when LED should return to idle state
// uint8_t led_active_state = LOW;       // LOW == LED on, HIGH == LED off
// uint8_t led_idle_state = HIGH;        // opposite of active
LED led;

uint8_t loop_id;

extern bool flashCRC;

struct GarageDoor garage_door;

extern "C" uint32_t __crc_len;
extern "C" uint32_t __crc_val;

// Track our memory usage
uint32_t free_heap = 65535;
uint32_t min_heap = 65535;
unsigned long next_heap_check = 0;

bool status_done = false;
unsigned long status_timeout;

/********************************** MAIN LOOP CODE *****************************************/

void setup()
{
    disable_extra4k_at_link_time();
    Serial.begin(115200);
    flashCRC = ESP.checkFlashCRC();
    LittleFS.begin();

    Serial.printf("\n"); // newline before we start
    led = LED();
    RINFO("=== Starting RATGDO Homekit version %s", AUTO_VERSION);
    RINFO("%s", ESP.getFullVersion().c_str());
    RINFO("Flash chip size 0x%X", ESP.getFlashChipSize());
    RINFO("Flash chip mode 0x%X", ESP.getFlashChipMode());
    RINFO("Flash chip speed 0x%X (%d MHz)", ESP.getFlashChipSpeed(), ESP.getFlashChipSpeed() / 1000000);
    // CRC checking starts at memory location 0x40200000, and proceeds until the address of __crc_len and __crc_val...
    // For CRC calculation purposes, those two long (32 bit) values are assumed to be zero.
    // The CRC calculation then proceeds until it get to 0x4020000 plus __crc_len.
    // Any memory writes/corruption within these blocks will cause checkFlashCRC() to fail.
    RINFO("Firmware CRC value: 0x%08X, CRC length: 0x%X (%d), Memory address of __crc_len,__crc_val: 0x%08X,0x%08X", __crc_val, __crc_len, __crc_len, &__crc_len, &__crc_val);
    if (flashCRC)
    {
        RINFO("checkFlashCRC: true");
    }
    else
    {
        RERROR("checkFlashCRC: false");
    }
    load_all_config_settings();
    wifi_connect();
    setup_web();
    if (!softAPmode)
    {
        setup_pins();
        setup_comms();
        setup_homekit();
    }

    led.idle();
    RINFO("=== RATGDO setup complete ===");
    RINFO("=============================");
    status_timeout = millis() + 2000;
}

void loop()
{
    improv_loop();
    comms_loop();
    // wait for a status command to be processes to properly set the initial state of
    // all homekit characteristics.  Also timeout if we don't receive a status in
    // a reasonable amount of time.  This prevents unintentional state changes if
    // a home hub reads the state before we initialize everything
    // Note, secplus1 doesnt have a status command so it will just timeout
    if (status_done)
    {
        homekit_loop();
    }
    else if (millis() > status_timeout)
    {
        RINFO("Status timeout, starting homekit");
        status_done = true;
    }
    service_timer_loop();
    web_loop();
    loop_id = LOOP_SYSTEM;
}

/*********************************** HELPER FUNCTIONS **************************************/

void setup_pins()
{
    RINFO("Setting up pins");

    pinMode(UART_TX_PIN, OUTPUT);
    pinMode(UART_RX_PIN, INPUT_PULLUP);

    pinMode(INPUT_OBST_PIN, INPUT);

    /*
     * TODO add support for dry contact switches
    pinMode(STATUS_DOOR_PIN, OUTPUT);
    */
    pinMode(STATUS_OBST_PIN, OUTPUT);
    /*
    pinMode(DRY_CONTACT_OPEN_PIN, INPUT_PULLUP);
    pinMode(DRY_CONTACT_CLOSE_PIN, INPUT_PULLUP);
    pinMode(DRY_CONTACT_LIGHT_PIN, INPUT_PULLUP);
    */

    /* pin-based obstruction detection
    // FALLING from https://github.com/ratgdo/esphome-ratgdo/blob/e248c705c5342e99201de272cb3e6dc0607a0f84/components/ratgdo/ratgdo.cpp#L54C14-L54C14
     */
    attachInterrupt(INPUT_OBST_PIN, isr_obstruction, FALLING);
}

/*********************************** MODEL **************************************/

/*************************** OBSTRUCTION DETECTION ***************************/
void IRAM_ATTR isr_obstruction()
{
    obstruction_sensor.low_count++;
}

void obstruction_timer()
{
    unsigned long current_millis = millis();
    static unsigned long last_millis = 0;

    // the obstruction sensor has 3 states: clear (HIGH with LOW pulse every 7ms), obstructed (HIGH), asleep (LOW)
    // the transitions between awake and asleep are tricky because the voltage drops slowly when falling asleep
    // and is high without pulses when waking up

    // If at least 3 low pulses are counted within 50ms, the door is awake, not obstructed and we don't have to check anything else

    const long CHECK_PERIOD = 50;
    const long PULSES_LOWER_LIMIT = 3;
    if (current_millis - last_millis > CHECK_PERIOD)
    {
        // check to see if we got more then PULSES_LOWER_LIMIT pulses
        if (obstruction_sensor.low_count > PULSES_LOWER_LIMIT)
        {
            // Only update if we are changing state
            if (garage_door.obstructed)
            {
                RINFO("Obstruction Clear");
                garage_door.obstructed = false;
                notify_homekit_obstruction();
                digitalWrite(STATUS_OBST_PIN, garage_door.obstructed);
                if (motionTriggers.bit.obstruction)
                {
                    garage_door.motion = false;
                    notify_homekit_motion();
                }
            }
        }
        else if (obstruction_sensor.low_count == 0)
        {
            // if there have been no pulses the line is steady high or low
            if (!digitalRead(INPUT_OBST_PIN))
            {
                // asleep
                obstruction_sensor.last_asleep = current_millis;
            }
            else
            {
                // if the line is high and was last asleep more than 700ms ago, then there is an obstruction present
                if (current_millis - obstruction_sensor.last_asleep > 700)
                {
                    // Only update if we are changing state
                    if (!garage_door.obstructed)
                    {
                        RINFO("Obstruction Detected");
                        garage_door.obstructed = true;
                        notify_homekit_obstruction();
                        digitalWrite(STATUS_OBST_PIN, garage_door.obstructed);
                        if (motionTriggers.bit.obstruction)
                        {
                            garage_door.motion = true;
                            notify_homekit_motion();
                        }
                    }
                }
            }
        }

        last_millis = current_millis;
        obstruction_sensor.low_count = 0;
    }
}

void service_timer_loop()
{
    loop_id = LOOP_TIMER;
    // Service the Obstruction Timer
    obstruction_timer();

    unsigned long current_millis = millis();

#ifdef NTP_CLIENT
    if (enableNTP && clockSet && lastRebootAt == 0)
    {
        lastRebootAt = time(NULL) - (current_millis / 1000);
        RINFO("System boot time: %s", timeString(lastRebootAt));
    }
#endif

    // LED flash timer
    led.flash();

    // Motion Clear Timer
    if (garage_door.motion && (current_millis > garage_door.motion_timer))
    {
        RINFO("Motion Cleared");
        garage_door.motion = false;
        notify_homekit_motion();
    }

    // Check heap
    if (current_millis > next_heap_check)
    {
        next_heap_check = current_millis + 1000;
        free_heap = ESP.getFreeHeap();
        if (free_heap < min_heap)
        {
            min_heap = free_heap;
            RINFO("Free heap dropped to %d", min_heap);
        }
    }
}

// Constructor for LED class
LED::LED()
{
    if (UART_TX_PIN != LED_BUILTIN)
    {
        // Serial.printf("Enabling built-in LED object\n");
        pinMode(LED_BUILTIN, OUTPUT);
        on();
    }
}

void LED::on()
{
    digitalWrite(LED_BUILTIN, LOW);
}

void LED::off()
{
    digitalWrite(LED_BUILTIN, HIGH);
}

void LED::idle()
{
    digitalWrite(LED_BUILTIN, idleState);
}

void LED::setIdleState(uint8_t state)
{
    idleState = state;
    activeState = (state == HIGH) ? LOW : HIGH;
}

void LED::flash(unsigned long ms)
{
    if (ms)
    {
        digitalWrite(LED_BUILTIN, activeState);
        resetTime = millis() + ms;
    }
    else if ((digitalRead(LED_BUILTIN) == activeState) && (millis() > resetTime))
    {
        digitalWrite(LED_BUILTIN, idleState);
    }
}
