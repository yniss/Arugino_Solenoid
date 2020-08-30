#include <Wire.h>
#include <LCD.h>
#include <LiquidCrystal_I2C.h>
#include <DS3231.h>
#include <EEPROM.h>
#include "arugino.h"

int BasePin    = 13; // output pin for NPN base
int ButtonPin  = 2;
int GatePin    = 8;
const int PumpOnUs    = 5000; //TODO: replace with SolenoidOnMs, also all places where solenoid is switched on
unsigned long previousMillis; // millis() returns unsigned long
unsigned long currentMillis;
int ButtonFlag = 0;
int toggle = 0;
DS3231 rtc(SDA, SCL);
LiquidCrystal_I2C lcd(LCD_ADDR,LCD_En_pin,LCD_Rw_pin,LCD_Rs_pin,LCD_D4_pin,LCD_D5_pin,LCD_D6_pin,LCD_D7_pin);

// Declare Mem Units stuff:
// EEPROM memory is 1024 bytes.
// Memory basic units are of Time Stamp, 8 bytes each.
// There are 1024/8 = 128 units.
// Memory split to the following allocations:
// 1. Summer watering programmable time stamps - for max of 7 watering slots | Units 0 - 6    (Addr: 0 - 55) //TODO: note that TS=0 should be reserved for not watering + need to check that this memory is initialized to 0
// 2. Winter   watering programmable time stamps - for max of 7 watering slots | Units 7 - 13 (Addr: 56 - 111) //TODO: note that TS=0 should be reserved for not watering + need to check that this memory is initialized to 0
// 3. History of last watering time stamps                                                                   | Units 14 - 125 (Addr: 112 -  10)
// 4. Last watering time stamp                                                                                     | Unit 126 (Addr: 1008 - 1015)
// 5. Current unit (in the history area)                                                                         | Unit 127 (Addr: 1016 - 1023)  // actually needs only 1 byte, but padded to a whole unit
// In order to access the units information easily - we'll hold an array of unit base addresses. 
uint8_t CurrMemUnit             = 0;                                                             // There are 128 units (of 8 bytes) in EEPROM (total 1024 bytes), so uint8_t is enough 
const int MemUnitLen            = 8;                                                             //  Tstamp - 8B
const int NumMemUnits         = 128; //EEPROM.length()/MemUnitLen;  // Total of 128 mem units
const int SeasonWateringUnitLen = 7;                                  // Max 7 Tstamp a week 
const int SummerWateringStartUnit = 0;  //TODO: set summer watering times and use it + set summmer/winter dates
const int WinterWateringStartUnit = SummerWateringStartUnit + SeasonWateringUnitLen;  //TODO: set winter watering times and use it
const int WateringHistStartUnit     = WinterWateringStartUnit + SeasonWateringUnitLen;  
const int WateringHistEndUnit     = EEPROM.length()/MemUnitLen-3; // Last 2 Memory units is reserved for Curr unit address & Last irrigation Tstamp
// 2 Special addresses:
const int MemAddr_WatrdTs    = EEPROM.length()-(2*MemUnitLen);  // Address of Last Irrigation's Tstamp
const int MemAddr_CurrUnit   = EEPROM.length()-(1*MemUnitLen);               // Address of Last written memory unit
int MemUnitArr[NumMemUnits];


void setup() {
  Serial.begin(9600);

  pinMode(BasePin, OUTPUT); 
  digitalWrite(BasePin, LOW); // default relay state
  pinMode(GatePin, OUTPUT); 
  digitalWrite(GatePin, LOW); // disable valve
  attachInterrupt(0, ButtonISR, FALLING); // set button interrupt - external int0
  previousMillis = millis();

  // Set RTC
  // Initialize the rtc object
  rtc.begin();
  // Uncomment for setting RTC time and date
  //rtc.setDOW(SATURDAY);     // Set Day-of-Week to SUNDAY
  //rtc.setTime(22, 22, 0);     // Set the time to 12:00:00 (24hr format)
  //rtc.setDate(8, 22, 2020);   // Set the date to January 1st, 2014
  // Print current RTC time  
  PrintTimeRTC();
  Serial.println();   

  // initialize current mem address
  //EEPROM.write(MemAddr_CurrUnit, WateringHistStartUnit); // AFTER 1ST TIME SHOULD BE COMMENTED (so that between log checks will not erase log)
  CurrMemUnit = (uint8_t)EEPROM.read(MemAddr_CurrUnit);
  MemUnitInit(MemUnitArr,0,NumMemUnits,MemUnitLen); // initialize MemUnitArr with unit addresses 

  // LCD init
  lcd.begin (16,2);
  // Switch on the backlight
  lcd.setBacklightPin(LCD_BACKLIGHT_PIN,POSITIVE);
  lcd.setBacklight(LOW);
  lcd.home (); // go home
}

void loop() {
  if (ButtonFlag == 1) { // if button was pressed - open valve for 5 seconds
    //TODO: temporary - each button press switch relay position
    // Enable/Disable valve - 30us pulse
    digitalWrite(GatePin, HIGH); // valve raise pulse high
    delay(30); 
    digitalWrite(GatePin, LOW); // valve drop pulse low
    if (toggle == 0) {
      Serial.println("DEBUG: BUTTON PRESSED at BasePin LOW - turning it HIGH");  
      //Opens the valve
      digitalWrite(BasePin, HIGH);
      toggle = 1;
      delay(1000);
      CurrMemUnit = MemWrite(CurrMemUnit);
      log_print();
      }
      else if (toggle == 1) {
        Serial.println("DEBUG: BUTTON PRESSED at BasePin HIGH - turning it LOW");  
        //Opens the valve
        digitalWrite(BasePin, LOW);
        toggle = 0;
        delay(1000);
      }
      ButtonFlag = 0; //Clear button flag
    }
}



//---------------//
//   EEPROM   //
//---------------//
// After declaring an array of Mem Units,
// we'll initialize the array with addresses.
void MemUnitInit(int MemUnitArr[], int BaseAddr, int ArrLen, int UnitLen) {
  for (int i=0; i<ArrLen; i++) {
    MemUnitArr[i] = BaseAddr + i*UnitLen;
  }
} 

//TODO: re-write no sensor
// MemWrite gets current mem unit. 
// It writes the watering time stamp to the current memory unit.
// It then writes current address in special mem address and returns it for next round.
uint8_t MemWrite(uint8_t CurrUnit)
{   
  if (CurrUnit > WateringHistEndUnit) // Check that we have enough space to write watering tstamp, otherwise go to beggining of watering history
    CurrUnit = WateringHistStartUnit;
  
  //  EEPROM.write(MemUnitArr[CurrUnit], Sens);                     // sensor value
  MemWriteTstamp((MemUnitArr[CurrUnit]));                       // time stamp
  //EEPROM.write((MemUnitArr[CurrUnit]+OFF_TANK), TankEmpty);     // water tank empty flag
  //EEPROM.write((MemUnitArr[CurrUnit]+OFF_WATRD), Watrd);        // watered flag
  CurrUnit++;
  EEPROM.write(MemAddr_CurrUnit, CurrUnit);                     // write curr unit in special mem address
  //if (Watrd == 1)
  MemWriteTstamp(MemAddr_WatrdTs);                            // time stamp of watering
  return CurrUnit;
}

void MemWriteTstamp(int addr)
{
  Time t = rtc.getTime();
  uint8_t t_year_byte [2];
  t_year_byte[0] = (t.year & 0xFF);
  t_year_byte[1] = (t.year >> 8);
  EEPROM.write(addr+OFF_DOW,    t.dow);             // DOW
  EEPROM.write(addr+OFF_DATE,   t.date);            // DATE
  EEPROM.write(addr+OFF_MON,    t.mon);             // MON
  EEPROM.write(addr+OFF_YEAR0,  t_year_byte[0]);    // YEAR (B0)
  EEPROM.write(addr+OFF_YEAR1,  t_year_byte[1]);    // YEAR (B1)
  EEPROM.write(addr+OFF_HOUR,   t.hour);            // HOUR
  EEPROM.write(addr+OFF_MIN,    t.min);             // MIN
  EEPROM.write(addr+OFF_SEC,    t.sec);             // SEC
}

Time MemReadTstamp(int addr)
{
  Time t;
  uint8_t t_year_byte [2];
  t_year_byte[0] = (t.year & 0xFF);
  t_year_byte[1] = (t.year >> 8);
  t.dow             = EEPROM.read(addr+OFF_DOW);    // DOW
  t.date            = EEPROM.read(addr+OFF_DATE);   // DATE
  t.mon             = EEPROM.read(addr+OFF_MON);    // MON
  t_year_byte[0]    = EEPROM.read(addr+OFF_YEAR0);  // YEAR (B0)
  t_year_byte[1]    = EEPROM.read(addr+OFF_YEAR1);  // YEAR (B1)
  t.hour            = EEPROM.read(addr+OFF_HOUR);   // HOUR
  t.min             = EEPROM.read(addr+OFF_MIN);    // MIN
  t.sec             = EEPROM.read(addr+OFF_SEC);    // SEC
  t.year            = ((uint16_t)t_year_byte[1] << 8) | t_year_byte[0];
  return t;
}


//------------------//
// Button Interrupt //
//------------------//
// TODO: will this work when arduino in sleep? should modify that interrupt wakes it first?
// Button reads and prints all moisture sensor values and tstamp written in memory.
// It also reads tstamp of last irrigation and prints it to LCD
void ButtonISR()
{
  static unsigned long last_interrupt_time = 0;
  unsigned long interrupt_time = millis();
  // If interrupts come faster than 200ms, assume it's a bounce and ignore
  if (interrupt_time - last_interrupt_time > 200) 
  {
    ButtonFlag = 1; // flag button was pressed, for irrigation print & LCD display
  }
  last_interrupt_time = interrupt_time;
}


//------------//
//     RTC    //
//------------//
void PrintTimeRTC()
  {
    // Send Day-of-Week
    Serial.print(rtc.getDOWStr());
    Serial.print(" ");
  
    // Send date
    Serial.print(rtc.getDateStr());
    Serial.print(" -- ");

    // Send time
    Serial.println(rtc.getTimeStr());
  }

//TODO: re-write no sensor
//------------//
//     LOG    //
//------------//
// log_print function prints to LCD last watering time stamp 
// and prints to serial monitor entire watering log
void log_print() //TODO: when called?
{
  int CurrUnit = EEPROM.read(MemAddr_CurrUnit);
  Time CurrTs;
  uint8_t CurrTsYearByte [2];
  char dt[16];
  char tm[16]; 
  //  uint8_t Watrd;//TODO
  //  uint8_t TankEmpty;//TODO
  
  ButtonFlag = 0; //TODO: why here?
  // LCD print
  lcd.setCursor ( 0, 0 );
  Time Watrd_ts = MemReadTstamp(MemAddr_WatrdTs); 
  char Watrd_dt[16];
  char Watrd_tm[16]; 
  // LCD - last irrigation time
  lcd.setBacklight(HIGH); // Backlight on
  sprintf(Watrd_dt, "%02d/%02d/%04d", Watrd_ts.date,Watrd_ts.mon,Watrd_ts.year);
  sprintf(Watrd_tm, "%02d:%02d:%02d", Watrd_ts.hour,Watrd_ts.min,Watrd_ts.sec);
  lcd.print(Watrd_dt);
  lcd.setCursor ( 0, 1 );
  lcd.print(Watrd_tm);
  delay(5000);
  lcd.setBacklight(LOW);  // Backlight off


  // Memory log print to serial monitor
  Serial.println("\n\n");
  Serial.println("Watering Log:");
  Serial.println("index\tDate\t\tTime"); //TODO: check indentation
  Serial.println("-----\t-----\t\t----"); //TODO: check indentation
  for (int i=WateringHistStartUnit; i<CurrUnit; i=i+1)
  {
    //    MemReadVal          = EEPROM.read(MemUnitArr[i]+OFF_SENS); //TODO
    CurrTs.dow          = EEPROM.read(MemUnitArr[i]+OFF_DOW);
    CurrTs.date         = EEPROM.read(MemUnitArr[i]+OFF_DATE);
    CurrTs.mon          = EEPROM.read(MemUnitArr[i]+OFF_MON);
    CurrTsYearByte[0]   = EEPROM.read(MemUnitArr[i]+OFF_YEAR0);
    CurrTsYearByte[1]   = EEPROM.read(MemUnitArr[i]+OFF_YEAR1);
    CurrTs.year         = ((uint16_t)CurrTsYearByte[1] << 8) | CurrTsYearByte[0];
    CurrTs.hour         = EEPROM.read(MemUnitArr[i]+OFF_HOUR);
    CurrTs.min          = EEPROM.read(MemUnitArr[i]+OFF_MIN);
    CurrTs.sec          = EEPROM.read(MemUnitArr[i]+OFF_SEC);
    //    TankEmpty           = EEPROM.read(MemUnitArr[i]+OFF_TANK); //TODO
    //Watrd               = EEPROM.read(MemUnitArr[i]+OFF_WATRD); //TODO
    sprintf(dt, "%02d/%02d/%04d", CurrTs.date,CurrTs.mon,CurrTs.year);
    sprintf(tm, "%02d:%02d:%02d", CurrTs.hour,CurrTs.min,CurrTs.sec);
    Serial.print(i-WateringHistStartUnit);   
    Serial.print("\t"); 
    //    Serial.print(MemReadVal); //TODO
    //Serial.print("\t"); 
    Serial.print(dt);
    Serial.print("\t");
    Serial.print(tm);
    Serial.print("  ");
    //    (TankEmpty==1) ? Serial.print("True") : Serial.print("False"); //TODO
    //Serial.print("\t    ");
    //(Watrd==1) ? Serial.print("True") : Serial.print("False"); //TODO
    Serial.println();
  }
  Serial.println("\n\n");
}
