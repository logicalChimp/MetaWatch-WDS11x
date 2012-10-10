//==============================================================================
//  Copyright 2011 Meta Watch Ltd. - http://www.MetaWatch.org/
// 
//  Licensed under the Meta Watch License, Version 1.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//  
//      http://www.MetaWatch.org/licenses/license-1.0.html
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//==============================================================================

/******************************************************************************/
/*! \file Accelerometer.c
*
*/
/******************************************************************************/

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "portmacro.h"

#include "hal_board_type.h"
#include "hal_accelerometer.h"
#include "Messages.h"
#include "hal_rtc.h"

#include "MessageQueues.h"     
#include "DebugUart.h"
#include "Utilities.h" 
#include "Accelerometer.h"
#include "Wrapper.h"

/******************************************************************************/
#define XYZ_DATA_LENGTH    (6)

static unsigned char WriteRegisterData;
static unsigned char pReadRegisterData[16];
static unsigned char Enabled = 0;
/******************************************************************************/

/* send interrupt only or send data (Send Interrupt Data [SID]) */
static unsigned char OperatingModeRegister;
static unsigned char InterruptControl;
static unsigned char SidControl;
static unsigned char SidAddr;
static unsigned char SidLength;

/******************************************************************************/

static void ReadInterruptReleaseRegister(void);

/******************************************************************************/

void InitializeAccelerometer(void)
{
  InitAccelerometerPeripheral();
  
  /* make sure accelerometer has had 20 ms to power up */
  TaskDelayLpmDisable();
  vTaskDelay(ACCELEROMETER_POWER_UP_TIME_MS);
  TaskDelayLpmEnable();
 
  PrintString("Accelerometer Initialization\r\n");
 
#if 0
  /* reset chip */
  WriteRegisterData = PC1_STANDBY_MODE;
  AccelerometerWrite(KIONIX_CTRL_REG1, &WriteRegisterData, ONE_BYTE);

  WriteRegisterData = SRST;
  AccelerometerWrite(KIONIX_CTRL_REG3, &WriteRegisterData, ONE_BYTE);
  
  /* wait until reset is complete */
  while ( WriteRegisterData & SRST )
  {
    AccelerometerRead(KIONIX_CTRL_REG3,&WriteRegisterData,ONE_BYTE);  
  }
#endif
  
  /* 
   * make sure part is in standby mode because some registers can only
   * be changed when the part is not active.
   */
  WriteRegisterData = PC1_STANDBY_MODE;
  AccelerometerWrite(KIONIX_CTRL_REG1, &WriteRegisterData, ONE_BYTE);

  /* enable face-up and face-down detection */
  WriteRegisterData = TILT_FDM | TILT_FUM;
  AccelerometerWrite(KIONIX_CTRL_REG2, &WriteRegisterData, ONE_BYTE);
    
  /* 
   * the interrupt from the accelerometer can be used to get periodic data
   * the real time clock can also be used
   */
  
  /* change to output data rate to 25 Hz */
  WriteRegisterData = WUF_ODR_25HZ | TAP_ODR_400HZ;
  AccelerometerWrite(KIONIX_CTRL_REG3, &WriteRegisterData, ONE_BYTE);
  
  /* enable interrupt and make it active high */
  WriteRegisterData = IEN | IEA;
  AccelerometerWrite(KIONIX_INT_CTRL_REG1, &WriteRegisterData, ONE_BYTE);
  
  /* enable motion detection interrupt for all three axis */
  WriteRegisterData = ZBW;
  AccelerometerWrite(KIONIX_INT_CTRL_REG2, &WriteRegisterData, ONE_BYTE);

  /* enable tap interrupt for Z-axis */
  WriteRegisterData = TFDM;
  AccelerometerWrite(KIONIX_INT_CTRL_REG3, &WriteRegisterData, ONE_BYTE);
  
  /* set TDT_TIMER to 0.2 secs*/
  WriteRegisterData = 0x50;
  AccelerometerWrite(KIONIX_TDT_TIMER, &WriteRegisterData, ONE_BYTE);
  
  /* set tap low and high thresholds (default: 26 and 182) */
  WriteRegisterData = 78;
  AccelerometerWrite(KIONIX_TDT_L_THRESH, &WriteRegisterData, ONE_BYTE);
  WriteRegisterData = 128;
  AccelerometerWrite(KIONIX_TDT_H_THRESH, &WriteRegisterData, ONE_BYTE);
    
  /* set WUF_TIMER counter */
  WriteRegisterData = 10;
  AccelerometerWrite(KIONIX_WUF_TIMER, &WriteRegisterData, ONE_BYTE);
    
  /* this causes data to always be sent */
  // WriteRegisterData = 0x00;
  WriteRegisterData = 0x08;
  AccelerometerWrite(KIONIX_WUF_THRESH, &WriteRegisterData, ONE_BYTE);
     
  /* single byte read test */
  AccelerometerRead(KIONIX_DCST_RESP,pReadRegisterData,1);
  PrintStringAndHex("KIONIX_DCST_RESP (0x55) = 0x",pReadRegisterData[0]);
  
  /* multiple byte read test */
  AccelerometerRead(KIONIX_WHO_AM_I,pReadRegisterData,2);
  PrintStringAndHex("KIONIX_WHO_AM_I (0x01) = 0x",pReadRegisterData[0]);
  PrintStringAndHex("KIONIX_TILT_POS_CUR (0x20) = 0x",pReadRegisterData[1]);  
    
  /* 
   * KIONIX_CTRL_REG3 and DATA_CTRL_REG can remain at their default values 
   *
   * 50 Hz
  */
#if 0  
  /* KTXF9 300 uA; KTXI9 165 uA */
  WriteRegisterData = PC1_OPERATING_MODE | TAP_ENABLE_TDTE;
  
  /* 180 uA; KTXI9 115 uA */
  WriteRegisterData = PC1_OPERATING_MODE | RESOLUTION_8BIT | WUF_ENABLE;

  /* 180 uA; KTXI9 8.7 uA */
  WriteRegisterData = PC1_OPERATING_MODE | TILT_ENABLE_TPE;

  /* 720 uA; KTXI9 330 uA */  
  WriteRegisterData = PC1_OPERATING_MODE | RESOLUTION_12BIT | WUF_ENABLE;
#endif
  
  /* setup the default for the AccelerometerEnable command */
  OperatingModeRegister = PC1_OPERATING_MODE | RESOLUTION_12BIT | 
    TAP_ENABLE_TDTE | TILT_ENABLE_TPE; // | WUF_ENABLE;
  InterruptControl = INTERRUPT_CONTROL_DISABLE_INTERRUPT; 
  SidControl = SID_CONTROL_SEND_DATA;
  SidAddr = KIONIX_XOUT_L;
  SidLength = XYZ_DATA_LENGTH;
  
  AccelerometerDisable();
  ACCELEROMETER_INT_ENABLE();
  
  PrintString("Accelerometer Init Complete\r\n");   
}


/* 
 * The interrupt can either send a message to the host or
 * it can send data (send a message that causes the task to read data from 
 * part and then send it to the host).
 */
void AccelerometerIsr(void)
{
#if 0
  /* disabling the interrupt is the easiest way to make sure that
   * the stack does not get blasted with
   * data when it is in sleep mode
   */
  ACCELEROMETER_INT_DISABLE();
#endif
  
  /* can't allocate buffer here so we must go to task to send interrupt
   * occurred message
   */
  tMessage Msg;
  SetupMessage(&Msg, AccelerometerSendDataMsg, NO_MSG_OPTIONS);  
  SendMessageToQueueFromIsr(BACKGROUND_QINDEX, &Msg);
}

static void ReadInterruptReleaseRegister(void)
{
#if 0
  /* interrupts are rising edge sensitive so clear and enable interrupt
   * before clearing it in the accelerometer 
   */
  ACCELEROMETER_INT_ENABLE();
#endif
  
  unsigned char temp;
  AccelerometerRead(KIONIX_INT_REL,&temp,1);
}

/* Send interrupt notification to the phone or 
 * read data from the accelerometer and send it to the phone
 */
void AccelerometerSendDataHandler(void)
{
  /* burst read */
  AccelerometerRead(KIONIX_TDT_TIMER, pReadRegisterData, 6);
  
  if (   pReadRegisterData[0] != 0x78 
      || pReadRegisterData[1] != 0xCB /* b6 */ 
      || pReadRegisterData[2] != 0x1A 
      || pReadRegisterData[3] != 0xA2 
      || pReadRegisterData[4] != 0x24 
      || pReadRegisterData[5] != 0x28 )
  {
    PrintString("Invalid i2c burst read\r\n");  
  }
          
  /* single read */
  AccelerometerRead(KIONIX_DCST_RESP,pReadRegisterData,1);
  
  if (pReadRegisterData[0] != 0x55)
  {
    PrintString("Invalid i2c Read\r\n"); 
  }

  AccelerometerRead(KIONIX_INT_SRC_REG2, pReadRegisterData, ONE_BYTE);

  tMessage Msg;
  if ((*pReadRegisterData & INT_TAP_SINGLE) == INT_TAP_SINGLE)
  {
//    InvertOption = (InvertOption == CONFIGURE_DISPLAY_OPTION_INVERT_DISPLAY) ? 
//      CONFIGURE_DISPLAY_OPTION_DONT_INVERT_DISPLAY : 
//      CONFIGURE_DISPLAY_OPTION_INVERT_DISPLAY;
    
//    SetupMessage(&Msg, ConfigureDisplay, InvertOption);
//    RouteMsg(&Msg);
  }
  else if ((*pReadRegisterData & INT_TAP_DOUBLE) == INT_TAP_DOUBLE)
  {
    SetupMessage(&Msg, LedChange, LED_TOGGLE_OPTION);
    RouteMsg(&Msg);
  }

  if (QueryPhoneConnected())
  {
    tMessage OutgoingMsg;

    if (SidControl == SID_CONTROL_SEND_INTERRUPT)
    {
      SetupMessageAndAllocateBuffer(&OutgoingMsg,
                            AccelerometerHostMsg,
                            ACCELEROMETER_HOST_MSG_IS_INTERRUPT_OPTION);
    }
    else
    {
      SetupMessageAndAllocateBuffer(&OutgoingMsg,
                                AccelerometerHostMsg,
                                ACCELEROMETER_HOST_MSG_IS_DATA_OPTION);

      OutgoingMsg.Length = SidLength;
      AccelerometerRead(SidAddr, OutgoingMsg.pBuffer, SidLength);

      // read orientation and tap status starting
      // AccelerometerReadSingle(KIONIX_INT_SRC_REG1, OutgoingMsg.pBuffer + SidLength);
      //*(OutgoingMsg.pBuffer + SidLength) = *pReadRegisterData;
      //OutgoingMsg.Length ++;
    }
    RouteMsg(&OutgoingMsg);
  }

  ReadInterruptReleaseRegister();
}

void AccelerometerEnable(void)
{
  /* put into the mode specified by the OperatingModeRegister */
  AccelerometerWrite(KIONIX_CTRL_REG1,&OperatingModeRegister,ONE_BYTE);
  
  if ( InterruptControl == INTERRUPT_CONTROL_ENABLE_INTERRUPT )
  {
    ReadInterruptReleaseRegister();
  }
  ACCELEROMETER_INT_ENABLE();
  Enabled = 1;
}

void AccelerometerDisable(void)
{   
  /* put into low power mode */
  WriteRegisterData = PC1_STANDBY_MODE;
  AccelerometerWrite(KIONIX_CTRL_REG1,&WriteRegisterData,ONE_BYTE);

  ACCELEROMETER_INT_DISABLE();
  Enabled = 0;
}

unsigned char QueryAccelerometerState(void)
{
  return Enabled;
}

/* 
 * Control how the msp430 responds to an interrupt,
 * control function of EnableAccelerometerMsg,
 * and allow enabling and disabling interrupt in msp430
 */
void AccelerometerSetupHandler(tMessage* pMsg)
{
  switch (pMsg->Options)
  {
  case ACCELEROMETER_SETUP_OPMODE_OPTION:
    OperatingModeRegister = pMsg->pBuffer[0];
    break;
  case ACCELEROMETER_SETUP_INTERRUPT_CONTROL_OPTION:
    InterruptControl = pMsg->pBuffer[0];
    break;
  case ACCELEROMETER_SETUP_SID_CONTROL_OPTION:
    SidControl = pMsg->pBuffer[0];
    break;
  case ACCELEROMETER_SETUP_SID_ADDR_OPTION:
    SidAddr = pMsg->pBuffer[0];
    break;
  case ACCELEROMETER_SETUP_SID_LENGTH_OPTION:
    SidLength = pMsg->pBuffer[0];
    break;
  case ACCELEROMETER_SETUP_INTERRUPT_ENABLE_DISABLE_OPTION:
    if ( pMsg->pBuffer[0] == 0 )
    {
      ACCELEROMETER_INT_DISABLE(); 
    }
    else
    {
      ACCELEROMETER_INT_ENABLE();  
    }
    break;
  default:
    PrintString("Unhandled Accelerometer Setup Option\r\n");
    break;
  }
}

/* Perform a read or write access of the accelerometer */
void AccelerometerAccessHandler(tMessage* pMsg)
{
  tAccelerometerAccessPayload* pPayload = 
    (tAccelerometerAccessPayload*) pMsg->pBuffer;

  if ( pMsg->Options == ACCELEROMETER_ACCESS_WRITE_OPTION )
  {
    AccelerometerWrite(pPayload->Address,&pPayload->Data,pPayload->Size);
  }
  else
  {
    tMessage OutgoingMsg;
    SetupMessageAndAllocateBuffer(&OutgoingMsg,
                                  AccelerometerResponseMsg,
                                  pPayload->Size);
    
    AccelerometerRead(pPayload->Address,
                      &OutgoingMsg.pBuffer[ACCELEROMETER_DATA_START_INDEX],
                      pPayload->Size);  
  }
}
