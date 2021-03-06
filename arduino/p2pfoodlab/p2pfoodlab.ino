/* 

   P2P Food Lab Sensorbox

   Copyright (C) 2013  Sony Computer Science Laboratory Paris
   Author: Peter Hanappe

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along wit
   h this program.  If not, see <http://www.gnu.org/licenses/>.

*/
#include <Wire.h>
#include <DHT22.h>
#include <Narcoleptic.h>
#include <SPI.h>

#include "FRAM.h"

#define DEBUG 1

#define SLAVE_ADDRESS           0x04
#define V_REF                   3.3f

#define DS1374_REG_TOD0		0x00 /* Time of Day */
#define DS1374_REG_TOD1		0x01
#define DS1374_REG_TOD2		0x02
#define DS1374_REG_TOD3		0x03
#define DS1374_REG_WDALM0	0x04 /* Watchdog/Alarm */
#define DS1374_REG_WDALM1	0x05
#define DS1374_REG_WDALM2	0x06
#define DS1374_REG_CR		0x07 /* Control */
#define DS1374_REG_CR_AIE	0x01 /* Alarm Int. Enable */
#define DS1374_REG_CR_WDALM	0x20 /* 1=Watchdog, 0=Alarm */
#define DS1374_REG_CR_WACE	0x40 /* WD/Alarm counter enable */
#define DS1374_REG_SR		0x08 /* Status */
#define DS1374_REG_SR_OSF	0x80 /* Oscillator Stop Flag */
#define DS1374_REG_SR_AF	0x01 /* Alarm Flag */
#define DS1374_REG_TCR		0x09 /* Trickle Charge */

#define CMD_POWEROFF            0x0a
#define CMD_SENSORS             0x0b
#define CMD_STATE               0x0c
#define CMD_FRAMES              0x0d
#define CMD_READ                0x0e
#define CMD_PUMP                0x0f
#define CMD_PERIOD              0x10
#define CMD_START               0x11
#define CMD_CHECKSUM            0x12
#define CMD_OFFSET              0x13
#define CMD_MEASURENOW          0x14
#define CMD_MEASUREMENT0        0x15
#define CMD_GETMEASUREMENT      0x16
#define CMD_RESTORESTACK        0x17
#define CMD_RESET               0xfe
#define CMD_DEBUG               0xff

#define DEBUG_STACK             (1 << 0)
#define DEBUG_STATE             (1 << 1)

#define STATE_MEASURING         0
#define STATE_RESETSTACK        1
#define STATE_SUSPEND           2

#define RHT03_1_PIN             4
#define RHT03_2_PIN             2
#define LUMINOSITY_PIN          A2
#define RPi_PIN                 9
#define BAT_USB_PIN             A3
#define PUMP_PIN                8

#define SENSOR_TRH              (1 << 0)
#define SENSOR_TRHX             (1 << 1)
#define SENSOR_LUM              (1 << 2)
#define SENSOR_USBBAT           (1 << 3)
#define SENSOR_SOIL             (1 << 4)

#define DEFAULT_SLEEP           20000

#if DEBUG
#define DebugPrint(_s) { Serial.println(_s); } 
#define DebugPrintValue(_s,_v) { Serial.print(_s); Serial.println(_v); } 
#else
#define DebugPrint(_s) 
#define DebugPrintValue(_s,_w)
#endif

typedef short sensor_value_t; 

/* The update period and poweroff/wakeup times are measured in
   minutes. */

typedef struct _shortstate_t {
        int sensors: 8;
        int suspend: 1;
        int linux_running: 1;
        int debug: 2;
        int reset_stack: 1;
        int measure_now: 1;
        unsigned char period; 
        unsigned short wakeup;
        unsigned char measurement_index; 
} shortstate_t;

typedef struct _longstate_t {
        int sensors: 8;
        int suspend: 1;
        int linux_running: 1;
        int debug: 2;
        int reset_stack: 1;
        unsigned char period; 
        unsigned short wakeup;
        unsigned char measure;
        unsigned char restore_stack;
        unsigned char reset;
        unsigned short poweroff;
        unsigned short read_index;
        unsigned long minutes;
        unsigned long suspend_start;
        unsigned char command;
        sensor_value_t measurements[10];
} longstate_t;

longstate_t state;
shortstate_t new_state;

DHT22 rht03_1(RHT03_1_PIN);
DHT22 rht03_2(RHT03_2_PIN);

// CRC-8 - based on the CRC8 formulas by Dallas/Maxim
// code released under the therms of the GNU GPL 3.0 license
// http://www.leonardomiliani.com/2013/un-semplice-crc8-per-arduino/?lang=en
static unsigned char crc8(unsigned char crc, const unsigned char *data, unsigned short len) 
{
        while (len--) {
                unsigned char extract = *data++;
                for (unsigned char i = 8; i; i--) {
                        unsigned char sum = (crc ^ extract) & 0x01;
                        crc >>= 1;
                        if (sum) {
                                crc ^= 0x8C;
                        }
                        extract >>= 1;
                }
        }
        return crc;
}

/*
 * Time functions
 */

static unsigned long _start = 0;

#define getmilliseconds()  (millis() + Narcoleptic.millis())

static unsigned long getseconds()
{
        static unsigned long _seconds = 0;
        unsigned long s = getmilliseconds() / 1000;

        while (s < _seconds)
                s += 4294967; // 2^32 milliseconds
        _seconds = s;
        return _seconds;
}

static unsigned long time(unsigned long t)
{
        unsigned long s = getseconds();
        if (t) {
                _start = t - s;
        }
        return _start + s;
}

#define getminutes() (getseconds() / 60)
#define hastime() (_start != 0)

/*
 * Stack 
 */

/* The timestamps and sensor data are pushed onto the stack until the
   RPi downloads it. */
#define STACK_SIZE 168

typedef struct _stack_t {
        unsigned long offset;
        unsigned short sp;
        unsigned short frames;
        unsigned char framesize;
        unsigned char checksum;
        unsigned char disabled;
        sensor_value_t values[STACK_SIZE];
} stack_t;

stack_t _stack;

static void stack_clear()
{
        _stack.disabled = 0;
        _stack.sp = 0;
        _stack.frames = 0;
        _stack.checksum = 0;
        _stack.offset = time(0);
}

#define stack_set_framesize(__framesize)  { _stack.framesize = __framesize; }
#define stack_frame_begin()               (_stack.sp)
#define stack_frame_unroll(__sp)          { _stack.sp = __sp; }
#define stack_address()                   ((unsigned char*) &_stack.values[0])
#define stack_bytesize()                  (_stack.frames * _stack.framesize * sizeof(sensor_value_t))
#define stack_frame_bytesize()            (_stack.framesize * sizeof(sensor_value_t))
#define stack_geti(__n)                   (_stack.values[__n].i)
#define stack_checksum()                  (_stack.checksum)
#define stack_offset()                    (_stack.offset)
#define stack_num_frames()                (_stack.frames)
#define stack_disable()                   { _stack.disabled = 1; }
#define stack_enable()                    { _stack.disabled = 0; }

static void stack_frame_end()
{
        if (!hastime()) 
                return; // skip

        /* This update should be called with interupts disabled!
           Without it, we might be sending back the wrong checksum
           and/or number of frames when an I2C request comes in. */
        unsigned char* data = stack_address();
        unsigned short offset = stack_bytesize();
        unsigned short len = stack_frame_bytesize();
        unsigned char crc = crc8(_stack.checksum, data + offset, len);
        _stack.checksum = crc;
        _stack.frames++;
}

static int stack_pushdate(unsigned long t)
{
        if (!hastime() || _stack.disabled) {
                return 1; // skip
        } else if (_stack.sp < STACK_SIZE) {
                _stack.values[_stack.sp++] = (sensor_value_t) ((t - _stack.offset) / 60);
                return 1;
        } else {
                DebugPrint("  STACK FULL");
                return 0;
        }
}

static int stack_push(short value)
{
        if (!hastime() || _stack.disabled) {
                return 1; // skip
        } else if (_stack.sp < STACK_SIZE) {
                _stack.values[_stack.sp++] = (sensor_value_t) value;
                return 1;
        } else {
                DebugPrint("  STACK FULL");
                return 0;
        }
}


/*
 * I2C interupt handlers
 */

static void receive_data(int len)
{
        unsigned char recv_buf[5];
        unsigned char recv_len = 0;
                
        for (int i = 0; i < len; i++) {
                int v = Wire.read();
                if (i < sizeof(recv_buf)) 
                        recv_buf[recv_len++] = v & 0xff;
                //Serial.println(v);
        }

        state.command = recv_buf[0];

        if ((state.command == DS1374_REG_TOD0) 
            && (recv_len == 5)) {
                unsigned long t;
                int hadtime = hastime();
                t = recv_buf[1];
                t = (t << 8) | recv_buf[2];
                t = (t << 8) | recv_buf[3];
                t = (t << 8) | recv_buf[4];
                time(t);
                if (!hadtime) stack_clear();

        } else if ((state.command == CMD_POWEROFF) 
                   && (recv_len == 3)) {
                new_state.wakeup = (recv_buf[1] << 8) | recv_buf[2];

        } else if ((state.command == CMD_SENSORS) 
                   && (recv_len == 2)) {
                new_state.sensors = recv_buf[1];

        } else if ((state.command == CMD_PERIOD) 
                   && (recv_len == 2)) {
                new_state.period = recv_buf[1];

        } else if ((state.command == CMD_STATE) 
                   && (recv_len == 2)) {
                new_state.reset_stack = (recv_buf[1] == STATE_RESETSTACK);
                new_state.suspend = (recv_buf[1] == STATE_SUSPEND);

        } else if (state.command == CMD_FRAMES) {
                // Do nothing here

        } else if (state.command == CMD_START) {
                state.read_index = 0;

        } else if (state.command == CMD_CHECKSUM) {
                // Do nothing here

        } else if (state.command == CMD_OFFSET) {
                // Do nothing here

        } else if (state.command == CMD_READ) {
                // Do nothing here

        } else if (state.command == CMD_PUMP) {
                // TODO

        } else if ((state.command == CMD_DEBUG) 
                   && (recv_len == 1)) {
                new_state.debug = recv_buf[1];

        } else if (state.command == CMD_MEASURENOW) {
                new_state.measure_now = 1;
                
        } else if (state.command == CMD_MEASUREMENT0) {
                new_state.measurement_index = 0;
                
        } else if (state.command == CMD_GETMEASUREMENT) { 
                // Do nothing here
        } else if (state.command == CMD_RESET) {
                state.reset = 1;
        }
}

static void send_data()
{
        unsigned char send_buf[4];
        unsigned char send_len = 0;

        if (state.command == DS1374_REG_TOD0) {
                unsigned long t = time(0);
                send_len = 4;
                send_buf[0] = (t & 0xff000000) >> 24;
                send_buf[1] = (t & 0x00ff0000) >> 16;
                send_buf[2] = (t & 0x0000ff00) >> 8;
                send_buf[3] = (t & 0x000000ff);

        } else if ((state.command == DS1374_REG_TOD1)
                   || (state.command == DS1374_REG_TOD2)
                   || (state.command == DS1374_REG_TOD3)) {
                send_len = 4;
                send_buf[0] = 0;
                send_buf[1] = 0;
                send_buf[2] = 0;
                send_buf[3] = 0;

        } else if ((state.command == DS1374_REG_WDALM0)
                   || (state.command == DS1374_REG_WDALM1)
                   || (state.command == DS1374_REG_WDALM2)) {
                send_len = 3;
                send_buf[0] = 0;
                send_buf[1] = 0;
                send_buf[2] = 0;

        } else if ((state.command == DS1374_REG_CR)
                   || (state.command == DS1374_REG_SR)) {
                send_len = 1;
                send_buf[0] = 0;

        } else if (state.command == CMD_POWEROFF) {
                send_len = 2;
                send_buf[0] = (state.wakeup & 0xff00) >> 8;
                send_buf[1] = (state.wakeup & 0x00ff);

        } else if (state.command == CMD_SENSORS) {
                send_len = 1;
                send_buf[0] = state.sensors;

        } else if (state.command == CMD_PERIOD) {
                send_len = 1;
                send_buf[0] = state.period;

        } else if (state.command == CMD_STATE) {
                send_len = 1;
                send_buf[0] = state.suspend;

        } else if (state.command == CMD_FRAMES) {
                send_len = 2;
                send_buf[0] = (stack_num_frames() & 0xff00) >> 8; 
                send_buf[1] = (stack_num_frames() & 0x00ff);

        } else if (state.command == CMD_START) {
                /* nothing to do here */

        } else if (state.command == CMD_READ) {
                unsigned char* ptr = stack_address();
                unsigned short len = stack_bytesize();
                send_len = sizeof(sensor_value_t);
                if ((len == 0) || (state.read_index >= len)) {
                        for (int k = 0; k < sizeof(sensor_value_t); k++)
                                send_buf[k] = 0;
                } else  {
                        for (int k = 0; k < sizeof(sensor_value_t); k++)
                                send_buf[k] = ptr[state.read_index++]; // FIXME: arbitrary handling of endianess...
                }

        } else if (state.command == CMD_PUMP) {

        } else if (state.command == CMD_CHECKSUM) {
                send_len = 1;
                send_buf[0] = stack_checksum();

        } else if (state.command == CMD_OFFSET) {
                unsigned long t = stack_offset();
                send_len = 4;
                send_buf[0] = (t & 0xff000000) >> 24;
                send_buf[1] = (t & 0x00ff0000) >> 16;
                send_buf[2] = (t & 0x0000ff00) >> 8;
                send_buf[3] = (t & 0x000000ff);

        } else if (state.command == CMD_GETMEASUREMENT) { 
                unsigned char* ptr = (unsigned char*) &state.measurements[new_state.measurement_index++];
                send_len = sizeof(sensor_value_t);
                for (int k = 0; k < sizeof(sensor_value_t); k++)
                        send_buf[k] = ptr[k]; // FIXME: arbitrary handling of endianess...
        } else if (state.command == CMD_RESTORESTACK) {
                send_len = 2;
                send_buf[0] = (FRAMFrameCounter & 0xff00) >> 8;
                send_buf[1] = (FRAMFrameCounter & 0x00ff);
                state.restore_stack = 1;
        }
        
        Wire.write(send_buf, send_len);
}

/*
 * Sensors & measurements
 */

static short get_level_usb_batttery()
{
        int a = analogRead(BAT_USB_PIN);
        //DebugPrintValue("  A3 ", a);
        float v = V_REF * 2.0f * a / 1024.0f;
        short r = (short) (100 * v);
        return r;
}

static short get_luminosity()
{
        return analogRead(LUMINOSITY_PIN);
}

static short get_soilhumidity()
{
        return 0;
}

static int get_rht03(DHT22* sensor, short* t, short* h)
{ 
        delay(2000);
  
        for (int i = 0; i < 10; i++) {
                DHT22_ERROR_t errorCode = sensor->readData();
                if (errorCode == DHT_ERROR_NONE) {
                        *t = 10 * sensor->getTemperatureCInt();
                        *h = 10 * sensor->getHumidityInt();
                        return 0;
                }
                DebugPrintValue("rht03 error: ", errorCode);
                delay(500);  
        }
        return -1;
}

static void measure_sensors()
{  

        DebugPrint("  measure");

        if (!hastime())
                DebugPrint("  *TIME NOT SET*");

        unsigned short old_sp = stack_frame_begin();
        unsigned char index = 0;
        
        if (state.suspend) {
                DebugPrint("  *SUSPENDED*");
                return;
        }

        if (!stack_pushdate(time(0)))
                goto unroll_stack;

        if (state.sensors & SENSOR_TRH) {
                short t, rh; 
                if (get_rht03(&rht03_1, &t, &rh) == 0) {
                        state.measurements[index++] = t;
                        state.measurements[index++] = rh;
                        if (!stack_push(t))
                                goto unroll_stack;
                        if (!stack_push(rh))
                                goto unroll_stack;
                        DebugPrintValue("  t ", t);
                        DebugPrintValue("  rh ", rh);
                } else {
                        if (!stack_push(-30000))
                                goto unroll_stack;
                        if (!stack_push(-1))
                                goto unroll_stack;
                }
        }
        if (state.sensors & SENSOR_TRHX) {
                short t, rh; 
                if (get_rht03(&rht03_2, &t, &rh) == 0) {
                        state.measurements[index++] = t;
                        state.measurements[index++] = rh;
                        if (!stack_push(t))
                                goto unroll_stack;
                        if (!stack_push(rh))
                                goto unroll_stack;
                        DebugPrintValue("  tx ", t);
                        DebugPrintValue("  rhx ", rh);
                } else {
                        if (!stack_push(-30000))
                                goto unroll_stack;
                        if (!stack_push(-1))
                                goto unroll_stack;
                }
        }

        if (state.sensors & SENSOR_LUM) {
                short luminosity = get_luminosity(); 
                state.measurements[index++] = luminosity;
                if (!stack_push(luminosity))
                        goto unroll_stack;
                DebugPrintValue("  lum ", luminosity);
        }

        if (state.sensors & SENSOR_USBBAT) {
                short lev = get_level_usb_batttery(); 
                state.measurements[index++] = lev;
                if (!stack_push(lev))
                        goto unroll_stack;
                DebugPrintValue("  usbbat ", lev);
        }

        if (state.sensors & SENSOR_SOIL) {
                short humidity = get_soilhumidity(); 
                state.measurements[index++] = humidity;
                if (!stack_push(humidity))
                        goto unroll_stack;
                DebugPrintValue("  soil ", humidity);
        }

        // Block I2C interupts
        noInterrupts();

        if (state.suspend) {
                /* We've been interrupted by an I2C mode change
                   request during the measurements. Roll back. */
                stack_frame_unroll(old_sp);
        } else {
                /* Update the frame count and the checksum. */
                stack_frame_end();
        }

        // Enable I2C interupts
        interrupts();

        return;

 unroll_stack:
        stack_frame_unroll(old_sp);
        // FIXME: possible infinite loop
        DebugPrint("  PUSHING STACK TO FRAM");
        if (FRAMWriteFrame((byte *)&_stack, sizeof(stack_t)) == 0)
        {
                stack_clear();
                measure_sensors();
        } else {
        DebugPrint("  FRAM FULL");
        }
        return;
}


/*
 * Utility functions
 */

static unsigned char count_sensors(unsigned char s)
{  
        unsigned char n = 0;
        if (s & SENSOR_TRH) 
                n += 2;
        if (s & SENSOR_TRHX)
                n += 2;
        if (s & SENSOR_LUM) 
                n += 1;
        if (s & SENSOR_USBBAT) 
                n += 1;
        if (s & SENSOR_SOIL)
                n += 1;
        return n;
}

static void blink(int count, int msec_on, int msec_off = 0)
{  
        int i;
        for (i = 0; i < count; i++) {
                digitalWrite(13, HIGH);
                delay(msec_on); 
                digitalWrite(13, LOW);
                delay(msec_off); 
        }                
}

static void print_state()
{  
        Serial.println(time(0)); 
        Serial.println(state.minutes); 
        Serial.println(_stack.sp); 
        Serial.println(_stack.frames); 
        Serial.println(state.period);
        Serial.println(state.suspend);
        Serial.println(state.measure);
        Serial.println(state.poweroff);
        Serial.println(state.wakeup);
        Serial.println();
}

static void print_stack()
{  
        int i;
        unsigned char* data = stack_address();
        int len = stack_bytesize();
        Serial.println("t:");
        for (i = 0; i < len; i++) {
                Serial.print(data[i], HEX);
                if ((i % 4) == 3)
                        Serial.println();
                else
                        Serial.print(" ");
        }
        if (((i-1) % 4) != 3)
                Serial.println();
        Serial.print("s:");
        Serial.println(stack_checksum(), HEX);
        Serial.print("o:");
        Serial.println(stack_offset());
}

static void handle_updates(unsigned long minutes)
{  
        if (state.sensors != new_state.sensors) {
                DebugPrintValue("  new sensor settings: ", new_state.sensors);
                state.sensors = new_state.sensors;
                stack_clear();
                stack_set_framesize(1 + count_sensors(state.sensors));
        }
        
        if (state.reset_stack != new_state.reset_stack) {
                state.reset_stack = new_state.reset_stack;
        }

        if (state.period != new_state.period) {
                DebugPrintValue("  new period: ", new_state.period);
                state.period = new_state.period;
        }

        if (state.suspend != new_state.suspend) {
                DebugPrintValue("  suspend: ", new_state.suspend);
                state.suspend = new_state.suspend;
                if (state.suspend) 
                        state.suspend_start = minutes;
                
        }

        if (new_state.wakeup != 0) {
                if (new_state.wakeup != state.wakeup) {
                        DebugPrintValue("  poweroff: wakeup at ", new_state.wakeup); 
                        state.poweroff = 2;
                        state.wakeup = new_state.wakeup;
                        if (state.wakeup < 3) 
                                state.wakeup = 3;
                }
                new_state.wakeup = 0;
        }
}

/*
 * Arduino main functions
 */

void setup() 
{
        Serial.begin(9600);

        pinMode(13, OUTPUT);
        digitalWrite(13, LOW);

        // initialize i2c as slave
        Wire.begin(SLAVE_ADDRESS);
        
        // define callbacks for i2c communication
        Wire.onReceive(receive_data);
        Wire.onRequest(send_data);

        // By default, start-up the RPi. The RPi will
        // tell the Arduino when to shut it down again.
        // However, first make sure the RPi was completely
        // shut down (pull down the pin) so it boots up correctly.
        pinMode(RPi_PIN, OUTPUT);

        digitalWrite(RPi_PIN, LOW);
        delay(1000);  
        digitalWrite(RPi_PIN, HIGH);


        pinMode(PUMP_PIN, OUTPUT);
        digitalWrite(PUMP_PIN, LOW);

        state.suspend = 0;
        state.sensors = SENSOR_TRH | SENSOR_TRHX | SENSOR_LUM | SENSOR_USBBAT;
        state.linux_running = 1;
        state.debug = 0;
        state.reset_stack = 0;
        state.period = 1;
        state.wakeup = 0;
        state.measure = 1;
        state.poweroff = 0;
        state.read_index = 0;
        state.minutes = getminutes();
        state.suspend_start = 0;
        state.command =  0xff;
        state.restore_stack = 0;
        state.reset = 0;

        new_state.suspend = 0;
        new_state.sensors =  SENSOR_TRH | SENSOR_TRHX | SENSOR_LUM | SENSOR_USBBAT;
        new_state.linux_running = 1;
        new_state.debug = 0;
        new_state.reset_stack = 0;
        
        new_state.period = 1;
        new_state.wakeup = 0;

        stack_clear();
        stack_set_framesize(1 + count_sensors(state.sensors));

        DebugPrint("Ready.");  
}

void loop()
{  
        unsigned long seconds = getseconds();
        unsigned long minutes = seconds / 60;
        unsigned long sleep = 0;

        handle_updates(minutes);

#if DEBUG
        delay(100);   
        print_state();
        delay(100);   
#endif

        if (Serial.available()) {
                int c = Serial.read();
                if (c == 'd') 
                        state.debug |= DEBUG_STACK | DEBUG_STATE;
                /* if (c == 'R') { */
                /*         unsigned char* ptr = stack_address(); */
                /*         unsigned short len = stack_bytesize(); */
                        
                /*         send_len = 4; */
                /*         if ((len == 0) || (state.read_index >= len)) { */
                /*                 send_buf[0] = 0; */
                /*                 send_buf[1] = 0; */
                /*                 send_buf[2] = 0; */
                /*                 send_buf[3] = 0; */
                /*         } else  { */
                /*                 send_buf[0] = ptr[state.read_index++]; */
                /*                 send_buf[1] = ptr[state.read_index++]; */
                /*                 send_buf[2] = ptr[state.read_index++]; */
                /*                 send_buf[3] = ptr[state.read_index++];  */
                /*         } */
                /* } */
        }

        if (state.reset)
        {
                asm volatile ("jmp 0");
        }

        if (state.restore_stack) {
                DebugPrint("  RESTORING STACK FROM FRAM");
                state.restore_stack = 0;
                // TODO: verify that the frame is not too long.
                FRAMReadFrame((byte *)&_stack);
        }

        if (state.debug & DEBUG_STATE) {
                print_state();
                state.debug &= ~DEBUG_STATE;
        }

        if (state.debug & DEBUG_STACK) {
                print_stack();
                state.debug &= ~DEBUG_STACK;
        }

        if (new_state.measure_now) {
                stack_disable();
                measure_sensors();
                stack_enable();
                new_state.measure_now = 0;
        }

        if (state.suspend) {
                
                /* Do short sleeps until the transfer is done. */
                sleep = 100;

                /* In case the data download failed and the arduino
                   was not resumed correctly, start measuring again
                   after one minute. */
                if (minutes - state.suspend_start > 3) {
                        DebugPrint("  TRANSFER TIMEOUT"); 
                        new_state.suspend = 0;
                }

        } else if (state.reset_stack) {
                
                /* Handle a stack reset request after a data transfer
                   or a change in the sensor configuration. */
                DebugPrint("  STACK RESET"); 
                stack_clear();
                state.reset_stack = 0;
                new_state.reset_stack = 0;

        } else {

                /* Measure (and other stuff) */

                while (state.minutes < minutes) {

                        if (state.measure > 0) {
                                state.measure--;
                                if (state.measure == 0) {
                                        blink(1, 100);
                                        measure_sensors();
                                        print_stack(); // DEBUG
                                        state.measure = state.period;
                                }
                        }
                        if (state.poweroff > 0) {
                                state.poweroff--;
                                if (state.poweroff == 0) {
                                        digitalWrite(RPi_PIN, LOW);
                                        DebugPrint("  POWEROFF"); 
                                        state.linux_running = 0;
                                }
                        }
                        if (state.wakeup > 0) {
                                state.wakeup--;
                                if (state.wakeup == 0) {
                                        digitalWrite(RPi_PIN, HIGH);
                                        DebugPrint("  WAKEUP"); 
                                        state.linux_running = 1;
                                }
                        }

                        state.minutes++;
                }

                sleep = DEFAULT_SLEEP;

        }

        if (sleep) {
                if (state.linux_running)
                        delay(sleep); 
                else 
                        Narcoleptic.delay(sleep); 
        }
}

/*
 * FRAM API functions
 */

// init the bus then clears the FRAM
void initFRAM()
{
        pinMode(FRAM_CS, OUTPUT);
        digitalWrite(FRAM_CS, HIGH);
        SPI.begin();
        SPI.setDataMode(SPI_MODE0);
        SPI.setBitOrder(MSBFIRST);
        SPI.setClockDivider(SPI_CLOCK_DIV2);
        // TODO: clear only if corrupted, else restore FRAM_frame_counter in ram.
        FRAMClear();
}

// clears the FRAM.
void FRAMClear()
{
        short zero = 0;
        FRAMWrite(FIRST_FRAME,   (byte *)&zero, 2);
        FRAMWrite(NEXT_FRAME,    (byte *)&zero, 2);
        FRAMWrite(FIFO_SIZE,     (byte *)&zero, 2);
        FRAMWrite(FRAME_COUNTER, (byte *)&zero, 2);
        FRAMFrameCounter = 0;
}

// copy one frame into (*frame) then deletes it from the FRAM.
// (*frame) must be big enough to hold the data
// returns 0 on success
//        -1 if no frames are available
//        -2 if the RAM is corupted
int FRAMReadFrame(byte *frame)
{
        unsigned short  firstFrame;
        unsigned short  fifoSize;
        unsigned short  frameCounter;
        unsigned char   crc;
        frame_header_t  header;

        FRAMRead(FRAME_COUNTER, (byte *)&frameCounter, 2);
        FRAMRead(FIFO_SIZE,     (byte *)&fifoSize, 2);
        if (fifoSize == 0)
                return -1;
        FRAMRead(FIRST_FRAME,   (byte *)&firstFrame, 2);
        FRAMCircularRead(firstFrame, (byte *)&header, FIFO_MAX_SIZE, HEADER_SIZE);
        FRAMCircularRead((firstFrame + HEADER_SIZE) % FIFO_MAX_SIZE, (byte *)frame, FIFO_MAX_SIZE, header.frameSize);
        crc = crc8(0, (unsigned char*)frame, header.frameSize);
        if (crc != header.checksum)
                return -2;
        fifoSize -= HEADER_SIZE + header.frameSize;
        firstFrame = (firstFrame + HEADER_SIZE + header.frameSize) % FIFO_MAX_SIZE;
        frameCounter--;
        FRAMWrite(FRAME_COUNTER, (byte *)&frameCounter, 2);
        FRAMWrite(FIRST_FRAME, (byte *)&firstFrame, 2);
        FRAMWrite(FIFO_SIZE, (byte *)&fifoSize, 2);
        FRAMFrameCounter = frameCounter;
        return 0;
}

// return the header of the next available frame.
// usefull to prepare a buffer of the right size.
// returns all fields to 0 if no frames are available.
frame_header_t FRAMReadframeHeader()
{
        unsigned short firstFrame;
        unsigned short fifoSize;
        frame_header_t header;

        header.frameSize = 0;
        header.checksum = 0;
        FRAMRead(FIFO_SIZE, (byte *)&fifoSize, 2);
        if (fifoSize == 0)
                return header;
        FRAMRead(FIRST_FRAME, (byte *)&firstFrame, 2);
        FRAMCircularRead(firstFrame, (byte *)&header, FIFO_MAX_SIZE, HEADER_SIZE);
        return header;
}

// return the ammount of frames curently stored in FRAM.
unsigned short FRAMReadFrameCounter()
{
        unsigned short frameCounter;

        FRAMRead(FRAME_COUNTER, (byte *)&frameCounter, 2);
        return frameCounter;
}

// Stores a buffer (*buf) of size (count) into a new frame.
// Returns 0 on success
//        -1 if FRAM is full.
int FRAMWriteFrame(byte *buf, unsigned short count)
{
        frame_header_t header;
        unsigned short fifoSize;
        unsigned short lastFrame;
        unsigned short frameCounter;

        FRAMRead(FIFO_SIZE,     (byte *)&fifoSize, 2);
        FRAMRead(NEXT_FRAME,    (byte *)&lastFrame, 2);
        FRAMRead(FRAME_COUNTER, (byte *)&frameCounter, 2);
        fifoSize = fifoSize + count + HEADER_SIZE;
        if (fifoSize > FIFO_MAX_SIZE)
                return -1;
        header.frameSize = count;
        header.checksum = crc8(0, (unsigned char*)buf, count);
        FRAMCircularWrite(lastFrame, (byte *)&header, FIFO_MAX_SIZE, HEADER_SIZE);
        FRAMCircularWrite((lastFrame + HEADER_SIZE) % FIFO_MAX_SIZE, (byte *)buf, FIFO_MAX_SIZE, count);
        lastFrame = (lastFrame + count + HEADER_SIZE) % FIFO_MAX_SIZE;
        frameCounter++;
        FRAMWrite(NEXT_FRAME, (byte *)&lastFrame, 2);
        FRAMWrite(FIFO_SIZE, (byte *)&fifoSize, 2);
        FRAMWrite(FRAME_COUNTER, (byte *)&frameCounter, 2);
        FRAMFrameCounter = frameCounter;
        return 0;
}

/*
 * FRAM internal functions
 */

// Write buffer (*buf) of size (count) at address (addr) in FRAM
// returns 0 on success
//        -1 on failure (invalid address)
int FRAMWrite(int addr, byte *buf, int count)
{
        noInterrupts();
        if (addr + count > FRAM_SIZE)
        {
                interrupts();
                return -1;
        }
        byte addrMSB = (addr >> 8) & 0xff;
        byte addrLSB = addr & 0xff;
        digitalWrite(FRAM_CS, LOW);
        SPI.transfer(CMD_WREN);
        digitalWrite(FRAM_CS, HIGH);
        digitalWrite(FRAM_CS, LOW);
        SPI.transfer(CMD_WRITE);
        SPI.transfer(addrMSB);
        SPI.transfer(addrLSB);
        for (int i = 0;i < count;i++)
                SPI.transfer(buf[i]);
        digitalWrite(FRAM_CS, HIGH);
        interrupts();
        return 0;
}

// Write buffer (*buf) of size (count) at address (addr) in FRAM
// returns 0 on success
//        -1 on failure (invalid address)
int FRAMRead(int addr, byte *buf, int count)
{
        noInterrupts();
        if (addr + count > FRAM_SIZE)
        {
                interrupts();
                return -1;
        }
        byte addrMSB = (addr >> 8) & 0xff;
        byte addrLSB = addr & 0xff;
        digitalWrite(FRAM_CS, LOW);
        SPI.transfer(CMD_READ);
        SPI.transfer(addrMSB);
        SPI.transfer(addrLSB);
        for (int i=0; i < count; i++)
                buf[i] = SPI.transfer(0x00);
        digitalWrite(FRAM_CS, HIGH);
        interrupts();
        return 0;
}

// Read and warps around if at the end of the FRAM
// returns 0 on success 
//        -1 if first read fails
//        -2 if seccond read fails
//        -3 if both reads fail.
int FRAMCircularRead(unsigned short addr, byte *buf, unsigned short maxAddr, int count)
{
        if (addr + count > maxAddr)
        {
                int ret = 0;
                int cut = maxAddr - addr;
                if (cut > 0)
                        ret = FRAMRead(addr, buf, cut);
                ret += FRAMRead(0, buf + cut, count - cut) * 2;
                return ret;
        }
        else
                return FRAMRead(addr, buf, count);
}

// Write and warps around if at the end of the FRAM
// returns 0 on success
//        -1 if first write fails
//        -2 if seccond write fails
//        -3 if both writes fail.
int FRAMCircularWrite(unsigned short addr, byte *buf, unsigned short maxAddr, int count)
{
        if (addr + count > maxAddr)
        {
                int ret = 0;
                int cut = maxAddr - addr;
                if (cut > 0)
                        ret =        FRAMWrite(addr, buf, cut);
                ret += FRAMWrite(0, buf + cut, count - cut) * 2;
                return ret;
        }
        else
                return FRAMWrite(addr, buf, count);
}


