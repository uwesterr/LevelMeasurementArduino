// I2C device class (I2Cdev) demonstration Arduino sketch for MPU6050 class using DMP (MotionApps v2.0)
// 6/21/2012 by Jeff Rowberg <jeff@rowberg.net>
// Updates should (hopefully) always be available at https://github.com/jrowberg/i2cdevlib
//
// Changelog:
//      2013-05-08 - added seamless Fastwire support
//                 - added note about gyro calibration
//      2012-06-21 - added note about Arduino 1.0.1 + Leonardo compatibility error
//      2012-06-20 - improved FIFO overflow handling and simplified read process
//      2012-06-19 - completely rearranged DMP initialization code and simplification
//      2012-06-13 - pull gyro and accel data from FIFO packet instead of reading directly
//      2012-06-09 - fix broken FIFO read sequence and change interrupt detection to RISING
//      2012-06-05 - add gravity-compensated initial reference frame acceleration output
//                 - add 3D math helper file to DMP6 example sketch
//                 - add Euler output and Yaw/Pitch/Roll output formats
//      2012-06-04 - remove accel offset clearing for better results (thanks Sungon Lee)
//      2012-06-01 - fixed gyro sensitivity to be 2000 deg/sec instead of 250
//      2012-05-30 - basic DMP initialization working

//      2020-03-10 - add EEROM write to store offsets when power off; as decribed in https://www.arduino.cc/en/Reference/EEPROMPut, changed by uwe

/* ============================================
I2Cdev device library code is placed under the MIT license
Copyright (c) 2012 Jeff Rowberg

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
===============================================
*/
#include <SPI.h>
#include <stdint.h>
#include <TFTv2.h>
#include <SeeedTouchScreen.h>
#include <EEPROM.h>

// I2Cdev and MPU6050 must be installed as libraries, or else the .cpp/.h files
// for both classes must be in the include path of your project
#include "I2Cdev.h"

#include "MPU6050_6Axis_MotionApps20.h"
//#include "MPU6050.h" // not necessary if using MotionApps include file

// Arduino Wire library is required if I2Cdev I2CDEV_ARDUINO_WIRE implementation
// is used in I2Cdev.h
#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
    #include "Wire.h"
#endif

// class default I2C address is 0x68
// specific I2C addresses may be passed as a parameter here
// AD0 low = 0x68 (default for SparkFun breakout and InvenSense evaluation board)
// AD0 high = 0x69
MPU6050 mpu;
//MPU6050 mpu(0x69); // <-- use for AD0 high


// uncomment "OUTPUT_READABLE_YAWPITCHROLL" if you want to see the yaw/
// pitch/roll angles (in degrees) calculated from the quaternions coming
// from the FIFO. Note this also requires gravity vector calculations.
// Also note that yaw/pitch/roll angles suffer from gimbal lock (for
// more info, see: http://en.wikipedia.org/wiki/Gimbal_lock)
#define OUTPUT_READABLE_YAWPITCHROLL


#define INTERRUPT_PIN 2  // use pin 2 on Arduino Uno & most boards
#define LED_PIN 13 // (Arduino is 13, Teensy is 11, Teensy++ is 6)
bool blinkState = false;

// MPU control/status vars
bool dmpReady = false;  // set true if DMP init was successful
uint8_t mpuIntStatus;   // holds actual interrupt status byte from MPU
uint8_t devStatus;      // return status after each device operation (0 = success, !0 = error)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount;     // count of all bytes currently in FIFO
uint8_t fifoBuffer[64]; // FIFO storage buffer

// orientation/motion vars
Quaternion q;           // [w, x, y, z]         quaternion container
VectorInt16 aa;         // [x, y, z]            accel sensor measurements
VectorInt16 aaReal;     // [x, y, z]            gravity-free accel sensor measurements
VectorInt16 aaWorld;    // [x, y, z]            world-frame accel sensor measurements
VectorFloat gravity;    // [x, y, z]            gravity vector
float euler[3];         // [psi, theta, phi]    Euler angle container
float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector

// packet structure for InvenSense teapot demo
uint8_t teapotPacket[14] = { '$', 0x02, 0,0, 0,0, 0,0, 0,0, 0x00, 0x00, '\r', '\n' };



// ================================================================
// ===               INTERRUPT DETECTION ROUTINE                ===
// ================================================================

volatile bool mpuInterrupt = false;     // indicates whether MPU interrupt pin has gone high
void dmpDataReady() {
    mpuInterrupt = true;
}


// include the library code:
#include <LiquidCrystal.h>

// initialize the library with the numbers of the interface pins
LiquidCrystal lcd(12, 11, 6, 5, 4, 3);

// ================================================================
// ===               Set variables for alignment measurement    ===
// ================================================================

float fMesswertPitch = 0.00;
float fMesswertRoll = 0.00;
float fPitchOffset = 0.0;
float fRollOffset = 0.0;
static char charMesswertPitch[15];
static char charMesswertRoll[15];
static char charRollOffset[15];
static char charPitchOffset[15];
/** the current address in the EEPROM (i.e. which byte we're going to write to next) **/
int addr = 0;


#define YP A2
#define XM A1
#define YM A0
#define XP A3

#define TS_MINX 116*2
#define TS_MAXX 890*2
#define TS_MINY 83*2
#define TS_MAXY 913*2

TouchScreen ts = TouchScreen(XP, YP, XM, YM);


// ================================================================
// ===                      INITIAL SETUP                       ===
// ================================================================

void setup() {
    // join I2C bus (I2Cdev library doesn't do this automatically)
    #if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
        Wire.begin();
       // Wire.setClock(400000); // 400kHz I2C clock. Comment this line if having compilation difficulties
    #elif I2CDEV_IMPLEMENTATION == I2CDEV_BUILTIN_FASTWIRE
        Fastwire::setup(400, true);
    #endif

    // initialize serial communication
    // (115200 chosen because it is required for Teapot Demo output, but it's
    // really up to you depending on your project)
    Serial.begin(115200);
    while (!Serial); // wait for Leonardo enumeration, others continue immediately

    // initialize device
    Serial.println(F("Initializing I2C devices..."));
    mpu.initialize();
    pinMode(INTERRUPT_PIN, INPUT);

    // verify connection
    Serial.println(F("Testing device connections..."));
    Serial.println(mpu.testConnection() ? F("MPU6050 connection successful") : F("MPU6050 connection failed"));


    // load and configure the DMP
    Serial.println(F("Initializing DMP..."));
    devStatus = mpu.dmpInitialize();

    // supply your own gyro offsets here, scaled for min sensitivity
    mpu.setXGyroOffset(220);
    mpu.setYGyroOffset(76);
    mpu.setZGyroOffset(-85);
    mpu.setZAccelOffset(1788); // 1688 factory default for my test chip

    // make sure it worked (returns 0 if so)
    if (devStatus == 0) {
        // turn on the DMP, now that it's ready
        Serial.println(F("Enabling DMP..."));
        mpu.setDMPEnabled(true);

        // enable Arduino interrupt detection
       // Serial.println(F("Enabling interrupt detection (Arduino external interrupt 0)..."));
        attachInterrupt(digitalPinToInterrupt(INTERRUPT_PIN), dmpDataReady, RISING);
        mpuIntStatus = mpu.getIntStatus();

        // set our DMP Ready flag so the main loop() function knows it's okay to use it
        Serial.println(F("DMP ready! Waiting for first interrupt..."));
        dmpReady = true;

        // get expected DMP packet size for later comparison
        packetSize = mpu.dmpGetFIFOPacketSize();
    } else {
        // ERROR!
        // 1 = initial memory load failed
        // 2 = DMP configuration updates failed
        // (if it's going to break, usually the code will be 1)
        Serial.print(F("DMP Initialization failed (code "));
        Serial.print(devStatus);
        Serial.println(F(")"));
    }

    // configure LED for output
    pinMode(LED_PIN, OUTPUT);

    // set up the LCD's number of columns and rows:
    lcd.begin(16, 2);

    TFT_BL_ON;      // turn on the background light
    Tft.TFTinit();  // init TFT library
    Tft.fillScreen(0, 240, 0, 320, BLUE);
    
    Tft.drawString("Messwerte", 0, 0, 4, WHITE);
    Tft.drawLine(2,35,230,35,WHITE);
    Tft.drawString("Pitch: ", 0, 50, 3, WHITE);
    Tft.drawString("Roll: ", 0, 80, 3, WHITE);
    Tft.drawString("Pitch Off: ", 0, 130, 2, WHITE);
    Tft.drawString("Roll  Off: ", 0, 160, 2, WHITE);

    
    Tft.drawLine(2,110,230,110,WHITE);
    // tft.drawLine(x0, y0, x1, y1, color)
    Tft.drawLine(180,200,60,200,YELLOW);
    Tft.drawLine(180,250,60,250,YELLOW);
    Tft.drawLine(60,200,60,250,YELLOW);
    Tft.drawLine(180,200,180,250,YELLOW);
    // x = 60...180
    // y = 200...250
    Tft.drawString("Zero", 80, 220, 3, WHITE);

    SPI.setClockDivider(128);

   // read offsets from EEPROM

    EEPROM.get(addr, fPitchOffset);
    dtostrf(fPitchOffset,7, 1, charPitchOffset); 
    dtostrf(fRollOffset,7, 1, charRollOffset); // convert float to string
    Tft.drawString(charPitchOffset, 100, 130, 2, GREEN);
    
    EEPROM.get(addr+sizeof(float), fRollOffset);
    dtostrf(fRollOffset,7, 1, charRollOffset); // convert float to string
    Tft.drawString(charRollOffset, 100, 160, 2, GREEN);    
    
}



// ================================================================
// ===                    MAIN PROGRAM LOOP                     ===
// ================================================================

void loop() {
    // if programming failed, don't try to do anything
    if (!dmpReady) return;

    // wait for MPU interrupt or extra packet(s) available
    while (!mpuInterrupt && fifoCount < packetSize) {
        // other program behavior stuff here
        // .
        // .
        // .
        // if you are really paranoid you can frequently test in between other
        // stuff to see if mpuInterrupt is true, and if so, "break;" from the
        // while() loop to immediately process the MPU data
        // .
        // .
        // .
        break;
    }

    // reset interrupt flag and get INT_STATUS byte
    mpuInterrupt = false;
    mpuIntStatus = mpu.getIntStatus();

    // get current FIFO count
    fifoCount = mpu.getFIFOCount();

    // check for overflow (this should never happen unless our code is too inefficient)
    if ((mpuIntStatus & 0x10) || fifoCount == 1024) {
        // reset so we can continue cleanly
        mpu.resetFIFO();
        Serial.println(F("FIFO overflow!"));

    // otherwise, check for DMP data ready interrupt (this should happen frequently)
    } else if (mpuIntStatus & 0x02) {
        // wait for correct available data length, should be a VERY short wait
        while (fifoCount < packetSize) fifoCount = mpu.getFIFOCount();

        // read a packet from FIFO
        mpu.getFIFOBytes(fifoBuffer, packetSize);
        
        // track FIFO count here in case there is > 1 packet available
        // (this lets us immediately read more without waiting for an interrupt)
        fifoCount -= packetSize;

        #ifdef OUTPUT_READABLE_QUATERNION
            // display quaternion values in easy matrix form: w x y z
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            Serial.print("quat\t");
            Serial.print(q.w);
            Serial.print("\t");
            Serial.print(q.x);
            Serial.print("\t");
            Serial.print(q.y);
            Serial.print("\t");
            Serial.println(q.z);
        #endif

        #ifdef OUTPUT_READABLE_EULER
            // display Euler angles in degrees
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            mpu.dmpGetEuler(euler, &q);
            Serial.print("euler\t");
            Serial.print(euler[0] * 180/M_PI);
            Serial.print("\t");
            Serial.print(euler[1] * 180/M_PI);
            Serial.print("\t");
            Serial.println(euler[2] * 180/M_PI);
        #endif

        #ifdef OUTPUT_READABLE_YAWPITCHROLL
            // display Euler angles in degrees
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            mpu.dmpGetGravity(&gravity, &q);
            mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);
            //Serial.print("ypr\t");
            //Serial.print(ypr[0] * 180/M_PI);
            //Serial.print("\t");
            //Serial.print(ypr[1] * 180/M_PI);
            //Serial.print("\t");
            //Serial.println(ypr[2] * 180/M_PI);
            lcd.setCursor(0, 0);
            lcd.print(String("Pitch: ") + String(ypr[1] * 180/M_PI));
            lcd.setCursor(0, 1);
            lcd.print(String("Roll: ") + String(ypr[2] * 180/M_PI));
            //delay(500);
            lcd.clear();

// ================================================================
// ===                    write measured data to screen        ===
// ================================================================
            // draw old values with background color to avoid multiple overwriting
            dtostrf(fMesswertPitch,7, 1, charMesswertPitch); // convert float to string
            dtostrf(fMesswertRoll,7, 1, charMesswertRoll);
            Tft.drawString(charMesswertPitch, 110, 50, 3, BLUE);
            Tft.drawString(charMesswertRoll, 110, 80, 3, BLUE);
            fMesswertPitch = ypr[1] * 180/M_PI-fPitchOffset;
            fMesswertRoll = ypr[2] * 180/M_PI-fRollOffset;
            dtostrf(fMesswertPitch,7, 1, charMesswertPitch); // convert float to string
            dtostrf(fMesswertRoll,7, 1, charMesswertRoll);
            //Tft.fillRectangle(109, 50, 120, 55, BLUE);
            if ((fMesswertPitch < -5.0) || (fMesswertPitch > 5.0))
              Tft.drawString(charMesswertPitch, 110, 50, 3, RED);
            if ((fMesswertPitch < 5.0) && (fMesswertPitch > -5.0))
              Tft.drawString(charMesswertPitch, 110, 50, 3, GREEN);
            if ((fMesswertRoll < -5.0) || (fMesswertRoll > 5.0))
              Tft.drawString(charMesswertRoll, 110, 80, 3, RED);
            if ((fMesswertRoll < 5.0) && (fMesswertRoll > -5.0))
              Tft.drawString(charMesswertRoll, 110, 80, 3, GREEN);
            delay(500);



// ================================================================
// ===         zero the alignment                               ===
// ================================================================

            Point p = ts.getPoint();
            p.x = map(p.x, TS_MINX, TS_MAXX, 0, 240);
            p.y = map(p.y, TS_MINY, TS_MAXY, 0, 320);
                // x = 60...180
                // y = 200...250
           if (p.z > 10)
            {
             if ((p.x > 60) &&(p.x < 180) && (p.y > 200) && (p.y < 250))
             {
             Tft.drawString(charMesswertPitch, 110, 50, 3, BLUE);
             Tft.drawString(charMesswertRoll, 110, 80, 3, BLUE);
             Tft.drawString("Zero", 270, 120, 3, GREEN);
             Tft.drawString(charPitchOffset, 100, 130, 2, BLUE);
             Tft.drawString(charRollOffset, 100, 160, 2, BLUE);
             fPitchOffset=ypr[1] * 180/M_PI;
             fRollOffset=ypr[2] * 180/M_PI;
             // write to EEPROM

              EEPROM.put(addr, fPitchOffset);
              EEPROM.put(addr+sizeof(float), fRollOffset);
             dtostrf(fPitchOffset,7, 1, charPitchOffset); 
             dtostrf(fRollOffset,7, 1, charRollOffset); // convert float to string
             Tft.drawString(charPitchOffset, 100, 130, 2, RED);
             Tft.drawString(charRollOffset, 100, 160, 2, RED);


             
             Tft.drawLine(180,200,60,200,RED);
             Tft.drawLine(180,250,60,250,RED);
             Tft.drawLine(60,200,60,250,RED);
             Tft.drawLine(180,200,180,250,RED);   
        
            delay(1000);
        
            Tft.drawString("Zero", 270, 120, 3, BLUE);
            Tft.drawString(charPitchOffset, 100, 130, 2, GREEN);
            Tft.drawString(charRollOffset, 100, 160, 2, GREEN);
            Tft.drawLine(180,200,60,200,YELLOW);
            Tft.drawLine(180,250,60,250,YELLOW);
            Tft.drawLine(60,200,60,250,YELLOW);
            Tft.drawLine(180,200,180,250,YELLOW);
             
            }  
            }

            Serial.print(p.z);
            

        #endif

        #ifdef OUTPUT_READABLE_REALACCEL
            // display real acceleration, adjusted to remove gravity
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            mpu.dmpGetAccel(&aa, fifoBuffer);
            mpu.dmpGetGravity(&gravity, &q);
            mpu.dmpGetLinearAccel(&aaReal, &aa, &gravity);
            Serial.print("areal\t");
            Serial.print(aaReal.x);
            Serial.print("\t");
            Serial.print(aaReal.y);
            Serial.print("\t");
            Serial.println(aaReal.z);
        #endif

        #ifdef OUTPUT_READABLE_WORLDACCEL
            // display initial world-frame acceleration, adjusted to remove gravity
            // and rotated based on known orientation from quaternion
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            mpu.dmpGetAccel(&aa, fifoBuffer);
            mpu.dmpGetGravity(&gravity, &q);
            mpu.dmpGetLinearAccel(&aaReal, &aa, &gravity);
            mpu.dmpGetLinearAccelInWorld(&aaWorld, &aaReal, &q);
            Serial.print("aworld\t");
            Serial.print(aaWorld.x);
            Serial.print("\t");
            Serial.print(aaWorld.y);
            Serial.print("\t");
            Serial.println(aaWorld.z);
        #endif
    
        #ifdef OUTPUT_TEAPOT
            // display quaternion values in InvenSense Teapot demo format:
            teapotPacket[2] = fifoBuffer[0];
            teapotPacket[3] = fifoBuffer[1];
            teapotPacket[4] = fifoBuffer[4];s
            teapotPacket[5] = fifoBuffer[5];
            teapotPacket[6] = fifoBuffer[8];
            teapotPacket[7] = fifoBuffer[9];
            teapotPacket[8] = fifoBuffer[12];
            teapotPacket[9] = fifoBuffer[13];
            Serial.write(teapotPacket, 14);
            teapotPacket[11]++; // packetCount, loops at 0xFF on purpose
        #endif

        // blink LED to indicate activity
        blinkState = !blinkState;
        digitalWrite(LED_PIN, blinkState);
    }
}
