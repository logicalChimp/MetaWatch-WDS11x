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
/*! \file LcdDisplay.c
 *
 */
/******************************************************************************/

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "Messages.h"

#include "hal_board_type.h"
#include "hal_rtc.h"
#include "hal_battery.h"
#include "hal_lcd.h"
#include "hal_lpm.h"

#include "DebugUart.h"
#include "Messages.h"
#include "Utilities.h"
#include "LcdDriver.h"
#include "Wrapper.h"
#include "MessageQueues.h"
#include "SerialRam.h"
#include "OneSecondTimers.h"
#include "Adc.h"
#include "Accelerometer.h"
#include "Buttons.h"
#include "Statistics.h"
#include "OSAL_Nv.h"
#include "Background.h"
#include "NvIds.h"
#include "Icons.h"
#include "Fonts.h"
#include "Display.h"
#include "LcdDisplay.h"

#define DISPLAY_TASK_QUEUE_LENGTH 8
#define DISPLAY_TASK_STACK_SIZE  	(configMINIMAL_STACK_SIZE + 90)
#define DISPLAY_TASK_PRIORITY     (tskIDLE_PRIORITY + 1)

#define IDLE_FULL_UPDATE   (0)
#define DATE_TIME_ONLY     (1)

#define BAR_CODE_START_ROW (27)
#define BAR_CODE_ROWS      (42)
#define SPLASH_START_ROW   (29)
#define SPLASH_ROWS        (32)

#define PAGE_TYPE_NUM      (3)
#define PAGE_TYPE_IDLE     (0)
#define PAGE_TYPE_MENU     (1)
#define PAGE_TYPE_INFO     (2)
#define PAGE_NUMBERS       (10)

#define BUTTON_NUMBERS     (5)
#define BTN_MSG            (0)
#define BTN_OPT            (1)

xTaskHandle DisplayHandle;

static void DisplayTask(void *pvParameters);

static void DisplayQueueMessageHandler(tMessage* pMsg);

static tMessage DisplayMsg;
static tTimerId DisplayTimerId;
static tTimerId LinkAlarmTimerId;
static unsigned char RtcUpdateEnable;
static unsigned char lastMin = 61;
/* Message handlers */

static void IdleUpdateHandler(unsigned char Options);
static void ChangeModeHandler(unsigned char Mode, unsigned char Options);
static void ModeTimeoutHandler();
static void WatchStatusScreenHandler(void);
static void BarCodeHandler(tMessage* pMsg);
static void ListPairedDevicesHandler(void);
static void ConfigureDisplayHandler(tMessage* pMsg);
static void ConfigureIdleBufferSizeHandler(tMessage* pMsg);
static void ModifyTimeHandler(tMessage* pMsg);
static void MenuModeHandler(unsigned char MsgOptions);
static void MenuButtonHandler(unsigned char MsgOptions);
static void ToggleSecondsHandler(unsigned char MsgOptions);
static void ConnectionStateChangeHandler(tMessage *pMsg);

/******************************************************************************/
static void DrawAnalogueTime(unsigned char OnceConnected);
static void DrawLine(int x0, int y0, int x1, int y1);
static void DrawDateTime(unsigned char OnceConnected);
static void DrawConnectionScreen(void);
static void InitMyBuffer(void);
static void DisplayStartupScreen(void);
static void SetupSplashScreenTimeout(void);
static void AllocateDisplayTimers(void);
static void StopDisplayTimer(void);
static void DetermineIdlePage(void);
static void DrawVersionInfo(unsigned char RowHeight);

static void DrawMenu1(void);
static void DrawMenu2(void);
static void DrawMenu3(void);
static void DrawCommonMenuIcons(void);

static void FillMyBuffer(unsigned char StartingRow,
                         unsigned char NumberOfRows,
                         unsigned char FillValue);

static void SendMyBufferToLcd(unsigned char StartingRow,
                              unsigned char NumberOfRows);

static void CopyRowsIntoMyBuffer(unsigned char const* pImage,
                                 unsigned char StartingRow,
                                 unsigned char NumberOfRows);

static void CopyColumnsIntoMyBuffer(unsigned char const* pImage,
                                    unsigned char StartingRow,
                                    unsigned char NumberOfRows,
                                    unsigned char StartingColumn,
                                    unsigned char NumberOfColumns);

static void WriteIcon4w10h(unsigned char const * pIcon,
                           unsigned char RowOffset,
                           unsigned char ColumnOffset);

static void DisplayAmPm(void);
//static void DisplayDayOfWeek(void);
static void DisplayDate(void);

/* the internal buffer */
#define STARTING_ROW                  ( 0 )
#define WATCH_DRAWN_IDLE_BUFFER_ROWS  ( 63 )
#define PHONE_IDLE_BUFFER_ROWS        ( 33 )

static tLcdLine pMyBuffer[NUM_LCD_ROWS];

const float sine_table[91] = {0, 0.01, 0.03, 0.05, 0.06, 0.08, 0.1, 0.12, 0.13, 0.15, 0.17, 0.19, 0.2, 0.22, 0.24, 0.26, 0.27, 0.29, 0.31, 0.33, 0.34, 0.36, 0.38, 0.4, 0.41, 0.43, 0.45, 0.47, 0.48, 0.5, 0.52, 0.54, 0.55, 0.57, 0.59, 0.61, 0.62, 0.64, 0.66, 0.68, 0.69, 0.71, 0.73, 0.75, 0.76, 0.78, 0.8, 0.82, 0.83, 0.85, 0.87, 0.89, 0.9, 0.92, 0.94, 0.95, 0.97, 0.99, 1.01, 1.02, 1.04, 1.06, 1.08, 1.09, 1.11, 1.13, 1.15, 1.16, 1.18, 1.2, 1.22, 1.23, 1.25, 1.27, 1.29, 1.3, 1.32, 1.34, 1.36, 1.37, 1.39, 1.41, 1.43, 1.44, 1.46, 1.48, 1.5, 1.51, 1.53, 1.55, 1.57};

/******************************************************************************/

static unsigned char nvIdleBufferConfig;
static unsigned char nvIdleBufferInvert;

static void SaveIdleBufferInvert(void);

/******************************************************************************/

unsigned char nvDisplaySeconds = 0;
static void SaveDisplaySeconds(void);

/******************************************************************************/

unsigned char CurrentMode = IDLE_MODE;

typedef enum
{
  NormalPage,
  /* the next three are only used on power-up */
  RadioOnWithPairingInfoPage,
  RadioOnWithoutPairingInfoPage,
  BluetoothOffPage,
  Menu1Page,
  Menu2Page,
  Menu3Page,
  ListPairedDevicesPage,
  WatchStatusPage,
  QrCodePage
} eIdleModePage;

static eIdleModePage CurrentPage[PAGE_TYPE_NUM];
static unsigned char PageType = PAGE_TYPE_IDLE;

static const unsigned char ButtonEvent[PAGE_NUMBERS][BUTTON_NUMBERS][2] =
{
  {{BarCode, 0}, {ToggleSecondsMsg, TOGGLE_SECONDS_OPTIONS_UPDATE_IDLE}, {MenuModeMsg, MENU_MODE_OPTION_PAGE1}, {ListPairedDevicesMsg, 0}, {WatchStatusMsg, 0}},
  {{BarCode, 0}, {ToggleSecondsMsg, TOGGLE_SECONDS_OPTIONS_UPDATE_IDLE}, {MenuModeMsg, MENU_MODE_OPTION_PAGE1}, {ListPairedDevicesMsg, 0}, {WatchStatusMsg, 0}},
  {{ModifyTimeMsg, MODIFY_TIME_INCREMENT_MINUTE}, {ModifyTimeMsg, MODIFY_TIME_INCREMENT_DOW}, {MenuModeMsg, MENU_MODE_OPTION_PAGE1}, {ListPairedDevicesMsg, 0}, {ModifyTimeMsg, MODIFY_TIME_INCREMENT_HOUR}},
  {{ModifyTimeMsg, MODIFY_TIME_INCREMENT_MINUTE}, {ModifyTimeMsg, MODIFY_TIME_INCREMENT_DOW}, {MenuModeMsg, MENU_MODE_OPTION_PAGE1}, {ListPairedDevicesMsg, 0}, {ModifyTimeMsg, MODIFY_TIME_INCREMENT_HOUR}},
  {{MenuButtonMsg, MENU_BUTTON_OPTION_TOGGLE_BLUETOOTH}, {MenuModeMsg, MENU_MODE_OPTION_PAGE2}, {MenuButtonMsg, MENU_BUTTON_OPTION_EXIT}, {MenuButtonMsg, MENU_BUTTON_OPTION_TOGGLE_LINK_ALARM}, {MenuButtonMsg, MENU_BUTTON_OPTION_TOGGLE_DISCOVERABILITY}},
  {{MenuButtonMsg, MENU_BUTTON_OPTION_TOGGLE_RST_NMI_PIN}, {MenuModeMsg, MENU_MODE_OPTION_PAGE3}, {MenuButtonMsg, MENU_BUTTON_OPTION_EXIT}, {MenuButtonMsg, MENU_BUTTON_OPTION_TOGGLE_SECURE_SIMPLE_PAIRING}, {SoftwareResetMsg, 0}},
  {{MenuButtonMsg, MENU_BUTTON_OPTION_TOGGLE_ACCEL}, {MenuButtonMsg, MENU_MODE_OPTION_PAGE1}, {MenuButtonMsg, MENU_BUTTON_OPTION_EXIT}, {MenuButtonMsg, MENU_BUTTON_OPTION_DISPLAY_SECONDS}, {MenuButtonMsg, MENU_BUTTON_OPTION_INVERT_DISPLAY}},
  {{BarCode, 0}, {0, 0}, {MenuModeMsg, MENU_MODE_OPTION_PAGE1}, {IdleUpdate, IDLE_FULL_UPDATE}, {WatchStatusMsg, 0}},
  {{BarCode, 0}, {0, 0}, {MenuModeMsg, MENU_MODE_OPTION_PAGE1}, {ListPairedDevicesMsg, 0}, {IdleUpdate, IDLE_FULL_UPDATE}},
  {{IdleUpdate, IDLE_FULL_UPDATE}, {0, 0}, {MenuModeMsg, MENU_MODE_OPTION_PAGE1}, {ListPairedDevicesMsg, 0}, {WatchStatusMsg, 0}}
};

static unsigned char SplashTimeout;

static void ConfigureIdleUserInterfaceButtons(void);

static void DontChangeButtonConfiguration(void);
static void DefaultApplicationAndNotificationButtonConfiguration(void);

/******************************************************************************/

const unsigned char pBarCodeImage[BAR_CODE_ROWS * NUM_LCD_COL_BYTES];
const unsigned char pMetaWatchSplash[SPLASH_ROWS * NUM_LCD_COL_BYTES];
const unsigned char Am[10*4];
const unsigned char Pm[10*4];
//const unsigned char DaysOfWeek[7][10*4];

/******************************************************************************/

static unsigned char gBitColumnMask;
static unsigned char gColumn;
static unsigned char gRow;

static void AdvanceBitColumnMask(unsigned int pixels);
static void WriteFontCharacter(unsigned char Character);
static void WriteFontString(tString* pString);

/******************************************************************************/

/*! Initialize the LCD display task
 *
 * Initializes the display driver, clears the display buffer and starts the
 * display task
 *
 * \return none, result is to start the display task
 */
void InitializeDisplayTask(void)
{
  InitMyBuffer();

  QueueHandles[DISPLAY_QINDEX] =
    xQueueCreate( DISPLAY_TASK_QUEUE_LENGTH, MESSAGE_QUEUE_ITEM_SIZE  );

  // task function, task name, stack len , task params, priority, task handle
  xTaskCreate(DisplayTask,
              (const signed char *)"DISPLAY",
              DISPLAY_TASK_STACK_SIZE,
              NULL,
              DISPLAY_TASK_PRIORITY,
              &DisplayHandle);

  ClearShippingModeFlag();
}

/*! LCD Task Main Loop
 *
 * \param pvParameters
 *
 */
static void DisplayTask(void *pvParameters)
{
  if ( QueueHandles[DISPLAY_QINDEX] == 0 )
  {
    PrintString("Display Queue not created!\r\n");
  }

  LcdPeripheralInit();
  DisplayStartupScreen();
  SerialRamInit();

  InitializeIdleBufferConfig();
  InitializeIdleBufferInvert();
  InitializeDisplaySeconds();
  InitializeLinkAlarmEnable();
  InitializeModeTimeouts();
  InitializeTimeFormat();
  InitializeDateFormat();
  AllocateDisplayTimers();
  SetupSplashScreenTimeout();

  DontChangeButtonConfiguration();
  DefaultApplicationAndNotificationButtonConfiguration();
  //SetupNormalIdleScreenButtons();

#ifndef ISOLATE_RADIO
  /* turn the radio on; initialize the serial port profile or BLE/GATT */
  tMessage Msg;
  SetupMessage(&Msg,TurnRadioOnMsg,NO_MSG_OPTIONS);
  RouteMsg(&Msg);
#endif

  for(;;)
  {
    if( pdTRUE == xQueueReceive(QueueHandles[DISPLAY_QINDEX],
                                &DisplayMsg, portMAX_DELAY) )
    {
      PrintMessageType(&DisplayMsg);
      DisplayQueueMessageHandler(&DisplayMsg);
      SendToFreeQueue(&DisplayMsg);
      CheckStackUsage(DisplayHandle, "Display");
      CheckQueueUsage(QueueHandles[DISPLAY_QINDEX]);
    }
  }
}

/*! Display the startup image or Splash Screen */
static void DisplayStartupScreen(void)
{
  /* draw metawatch logo */
  FillMyBuffer(STARTING_ROW, NUM_LCD_ROWS, 0x00);
  CopyRowsIntoMyBuffer(pMetaWatchSplash, SPLASH_START_ROW, SPLASH_ROWS);
  SendMyBufferToLcd(STARTING_ROW, NUM_LCD_ROWS);
}

/*! Handle the messages routed to the display queue */
static void DisplayQueueMessageHandler(tMessage* pMsg)
{
  switch(pMsg->Type)
  {
  case WriteBuffer:
    WriteBufferHandler(pMsg);
    break;

  case LoadTemplate:
    LoadTemplateHandler(pMsg);
    break;

  case UpdateDisplay:
    UpdateDisplayHandler(pMsg);
    break;

  case IdleUpdate:
      IdleUpdateHandler(pMsg->Options);
    break;

  case ChangeModeMsg:
    ChangeModeHandler(pMsg->Options & MODE_MASK, DATE_TIME_ONLY);
    break;

  case ModeTimeoutMsg:
    ModeTimeoutHandler();
    break;

  case WatchStatusMsg:
    WatchStatusScreenHandler();
    break;

  case BarCode:
    BarCodeHandler(pMsg);
    break;

  case ListPairedDevicesMsg:
    ListPairedDevicesHandler();
    break;

  case WatchDrawnScreenTimeout:
    IdleUpdateHandler(pMsg->Options);
    break;

  case ConfigureDisplay:
    ConfigureDisplayHandler(pMsg);
    break;

  case ConfigureIdleBufferSize:
    ConfigureIdleBufferSizeHandler(pMsg);
    break;

  case ConnectionStateChangeMsg:
    if (SplashTimeout) ConnectionStateChangeHandler(pMsg);
    break;

  case ModifyTimeMsg:
    ModifyTimeHandler(pMsg);
    break;

  case MenuModeMsg:
    MenuModeHandler(pMsg->Options);
    break;

  case MenuButtonMsg:
    MenuButtonHandler(pMsg->Options);
    break;

  case ToggleSecondsMsg:
    ToggleSecondsHandler(pMsg->Options);
    break;

  case SplashTimeoutMsg:
    SplashTimeout = 1;
//    DisplayDisconnectWarning = 0;
    DetermineIdlePage();
    IdleUpdateHandler(IDLE_FULL_UPDATE);
    break;

  case LowBatteryWarningMsg:
  case LowBatteryBtOffMsg:
    break;

  case LinkAlarmMsg:
    if ( QueryLinkAlarmEnable() )
    {
      GenerateLinkAlarm();
    }
//    DisplayDisconnectWarning = 1;

    SetupOneSecondTimer(LinkAlarmTimerId,
                        ONE_SECOND*5,
                        NO_REPEAT,
                        DISPLAY_QINDEX,
                        SplashTimeoutMsg,
                        NO_MSG_OPTIONS);

    StartOneSecondTimer(LinkAlarmTimerId);

    IdleUpdateHandler(DATE_TIME_ONLY);
    break;

  case RamTestMsg:
    RamTestHandler(pMsg);
    break;

  default:
    PrintStringAndHex("<<Unhandled Message>> Type: 0x", pMsg->Type);
    break;
  }
}

/*! Allocate ids and setup timers for the display modes */
static void AllocateDisplayTimers(void)
{
  DisplayTimerId = AllocateOneSecondTimer();
  LinkAlarmTimerId = AllocateOneSecondTimer();
}

static void SetupSplashScreenTimeout()
{
  SetupOneSecondTimer(DisplayTimerId,
                      ONE_SECOND*3,
                      NO_REPEAT,
                      DISPLAY_QINDEX,
                      SplashTimeoutMsg,
                      NO_MSG_OPTIONS);

  StartOneSecondTimer(DisplayTimerId);
}

static inline void StopDisplayTimer(void)
{
  RtcUpdateEnable = 0;
  StopOneSecondTimer(DisplayTimerId);
}

/*! Draw the Idle screen and cause the remainder of the display to be updated
 * also
 */
static void IdleUpdateHandler(unsigned char Options)
{
  StopDisplayTimer();

  /* allow rtc to send IdleUpdate every minute (or second) */
  RtcUpdateEnable = 1;

  if (nvIdleBufferConfig == WATCH_CONTROLS_TOP)
  {
    /* draw the date & time area */
    DrawDateTime(OnceConnected());
  }
  if (Options == DATE_TIME_ONLY) return;
  
  if (OnceConnected())
  {
    tMessage Message;
    SetupMessage(&Message, UpdateDisplay, IDLE_MODE | FORCE_UPDATE);
    RouteMsg(&Message);
  }
  else
  {
    DrawConnectionScreen();
  }
  
  PageType = PAGE_TYPE_IDLE;
  ConfigureIdleUserInterfaceButtons();
}

static void ChangeModeHandler(unsigned char Mode, unsigned char Options)
{
  //PrintStringAndTwoDecimals("Changing mode from ", CurrentMode, " to ", Mode);
  CurrentMode = Mode;

  if (Mode == IDLE_MODE)
  { 
    PageType = PAGE_TYPE_IDLE;
    //CurrentPage[PAGE_TYPE_IDLE] = RadioOnWithPairingInfoPage;
    IdleUpdateHandler(Options);
    if (Options == DATE_TIME_ONLY) ConfigureIdleUserInterfaceButtons();  
  }
  else
  {
    StopDisplayTimer();
    SetupOneSecondTimer(DisplayTimerId,
                        QueryModeTimeout(Mode),
                        NO_REPEAT,
                        DISPLAY_QINDEX,
                        ModeTimeoutMsg,
                        Mode);

    StartOneSecondTimer(DisplayTimerId);
  }

  /*
   * send a message to the host indicating buffer update / mode change
   * has completed.
   */
  tMessage Message;
  SetupMessageAndAllocateBuffer(&Message, StatusChangeEvent, Mode);
  Message.pBuffer[0] = eScUpdateComplete;
  Message.Length = 1;
  RouteMsg(&Message);
}

static void ModeTimeoutHandler()
{
  /* send a message to the host indicating that a timeout occurred */
  tMessage Message;
  SetupMessageAndAllocateBuffer(&Message, StatusChangeEvent, CurrentMode);
  Message.pBuffer[0] = eScModeTimeout;
  Message.Length = 1;
  RouteMsg(&Message);
  
  ChangeModeHandler(IDLE_MODE, IDLE_FULL_UPDATE);
}

static void ConnectionStateChangeHandler(tMessage *pMsg)
{
  //decide which idle page to be
  DetermineIdlePage();
  
  if (CurrentMode != IDLE_MODE)
  {
    if ((pMsg->Options == LEConnected ||
        pMsg->Options == BRConnected) &&
        QueryConnectionState() == Paired)
    {
      // clear button definition
      //PrintStringAndDecimal("+++ Clean Button: ", CurrentMode);
      CleanButtonCallbackOptions(CurrentMode);
    }
    ChangeModeHandler(IDLE_MODE, IDLE_FULL_UPDATE);
  }
  else
  {
    if (PageType == PAGE_TYPE_IDLE)
    {
      if (CurrentPage[PAGE_TYPE_IDLE] == NormalPage ||
          CurrentPage[PAGE_TYPE_IDLE] == RadioOnWithPairingInfoPage) 
        IdleUpdateHandler(DATE_TIME_ONLY);
        
      else DrawConnectionScreen();
      
      ConfigureIdleUserInterfaceButtons();
    }
    else if (PageType == PAGE_TYPE_MENU)
    {
      MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    }
    else if (CurrentPage[PAGE_TYPE_INFO] == ListPairedDevicesPage)
    {
      ListPairedDevicesHandler();
    }
    else if (CurrentPage[PAGE_TYPE_INFO] == WatchStatusPage)
    {
      WatchStatusScreenHandler();
    }
  }
}

static void DetermineIdlePage(void)
{
  etConnectionState cs = QueryConnectionState();
  
  if (OnceConnected()) 
    CurrentPage[PAGE_TYPE_IDLE] = NormalPage;
    
  else 
  {
    if (cs == RadioOn) 
      CurrentPage[PAGE_TYPE_IDLE] = QueryValidPairingInfo() ? 
        RadioOnWithPairingInfoPage : RadioOnWithoutPairingInfoPage;
      
    else if (cs == Paired) 
      CurrentPage[PAGE_TYPE_IDLE] = RadioOnWithoutPairingInfoPage;
      
    else CurrentPage[PAGE_TYPE_IDLE] = BluetoothOffPage;
  }
}

static void MenuModeHandler(unsigned char MsgOptions)
{
  StopDisplayTimer();

  /* draw entire region */
  FillMyBuffer(STARTING_ROW,PHONE_IDLE_BUFFER_ROWS,0x00);
  PageType = PAGE_TYPE_MENU;
  
  switch (MsgOptions)
  {
  case MENU_MODE_OPTION_PAGE1:
    DrawMenu1();
    CurrentPage[PAGE_TYPE_MENU] = Menu1Page;
    ConfigureIdleUserInterfaceButtons();
    break;

  case MENU_MODE_OPTION_PAGE2:
    DrawMenu2();
    CurrentPage[PAGE_TYPE_MENU] = Menu2Page;
    ConfigureIdleUserInterfaceButtons();
    break;

  case MENU_MODE_OPTION_PAGE3:
    DrawMenu3();
    CurrentPage[PAGE_TYPE_MENU] = Menu3Page;
    ConfigureIdleUserInterfaceButtons();
    break;

  case MENU_MODE_OPTION_UPDATE_CURRENT_PAGE:

  default:
    switch ( CurrentPage[PAGE_TYPE_MENU] )
    {
    case Menu1Page:
      DrawMenu1();
      break;
    case Menu2Page:
      DrawMenu2();
      break;
    case Menu3Page:
      DrawMenu3();
      break;
    default:
      PrintString("Menu Mode Screen Selection Error\r\n");
      break;
    }
    break;
  }

  /* these icons are common to all menus */
  DrawCommonMenuIcons();

  /* only invert the part that was just drawn */
  SendMyBufferToLcd(STARTING_ROW, NUM_LCD_ROWS);
}

static void MenuButtonHandler(unsigned char MsgOptions)
{
  StopDisplayTimer();

  tMessage OutgoingMsg;

  switch (MsgOptions)
  {
  case MENU_BUTTON_OPTION_TOGGLE_DISCOVERABILITY:

    if ( QueryConnectionState() != Initializing )
    {
      SetupMessage(&OutgoingMsg,PairingControlMsg,NO_MSG_OPTIONS);

      OutgoingMsg.Options = QueryDiscoverable() ? 
        PAIRING_CONTROL_OPTION_DISABLE_PAIRING : PAIRING_CONTROL_OPTION_ENABLE_PAIRING;

      RouteMsg(&OutgoingMsg);
    }
    /* screen will be updated with a message from spp */
    break;

  case MENU_BUTTON_OPTION_TOGGLE_LINK_ALARM:
    ToggleLinkAlarmEnable();
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;

  case MENU_BUTTON_OPTION_EXIT:

    /* save all of the non-volatile items */
    SetupMessage(&OutgoingMsg,PairingControlMsg,PAIRING_CONTROL_OPTION_SAVE_SPP);
    RouteMsg(&OutgoingMsg);

    SaveLinkAlarmEnable();
    SaveRstNmiConfiguration();
    SaveIdleBufferInvert();
    SaveDisplaySeconds();

    /* go back to the idle screen */
    PageType = PAGE_TYPE_IDLE;
    IdleUpdateHandler(IDLE_FULL_UPDATE);
    break;

  case MENU_BUTTON_OPTION_TOGGLE_BLUETOOTH:

    if ( QueryConnectionState() != Initializing )
    {
      SetupMessage(&OutgoingMsg,QueryBluetoothOn() ? TurnRadioOffMsg : TurnRadioOnMsg, NO_MSG_OPTIONS);
      RouteMsg(&OutgoingMsg);
    }
    /* screen will be updated with a message from spp */
    break;

  case MENU_BUTTON_OPTION_TOGGLE_SECURE_SIMPLE_PAIRING:
    if ( QueryConnectionState() != Initializing )
    {
      SetupMessage(&OutgoingMsg,PairingControlMsg,PAIRING_CONTROL_OPTION_TOGGLE_SSP);
      RouteMsg(&OutgoingMsg);
    }
    /* screen will be updated with a message from spp */
    break;

  case MENU_BUTTON_OPTION_TOGGLE_RST_NMI_PIN:
    ConfigRstPin(RST_PIN_TOGGLED);
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;

  case MENU_BUTTON_OPTION_DISPLAY_SECONDS:
    ToggleSecondsHandler(TOGGLE_SECONDS_OPTIONS_DONT_UPDATE_IDLE);
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;

  case MENU_BUTTON_OPTION_INVERT_DISPLAY:
    nvIdleBufferInvert = (nvIdleBufferInvert + 1) % 4;
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;

  case MENU_BUTTON_OPTION_TOGGLE_ACCEL:

    SetupMessage(
      &OutgoingMsg, 
      QueryAccelerometerState() ? AccelerometerDisableMsg : AccelerometerEnableMsg, 
      NO_MSG_OPTIONS);

    RouteMsg(&OutgoingMsg);
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;

  default:
    break;
  }
}

static void DontChangeButtonConfiguration(void)
{
  /* assign LED button to all modes */
  unsigned char i;
  for ( i = 0; i < NUMBER_OF_MODES; i++ )
  {
    /* turn off led 3 seconds after button has been released */
    DefineButtonAction(i,
                       SW_D_INDEX,
                       BUTTON_STATE_PRESSED,
                       LedChange,
                       LED_START_OFF_TIMER);

    /* turn on led immediately when button is pressed */
    DefineButtonAction(i,
                       SW_D_INDEX,
                       BUTTON_STATE_IMMEDIATE,
                       LedChange,
                       LED_ON_OPTION);

    /* software reset is available in all modes */
    DefineButtonAction(i,
                       SW_F_INDEX,
                       BUTTON_STATE_LONG_HOLD,
                       SoftwareResetMsg,
                       MASTER_RESET_OPTION);
  }
}

static void ConfigureIdleUserInterfaceButtons(void)
{
  /* only allow reset on one of the pages */
  DisableButtonAction(IDLE_MODE, SW_F_INDEX, BUTTON_STATE_PRESSED);
  
  unsigned char PageNo = CurrentPage[PageType];
  unsigned char i;
  for (i = 0; i < BUTTON_NUMBERS; i ++)
  {
    if (ButtonEvent[PageNo][i][BTN_MSG])
    {
      DefineButtonAction(
        IDLE_MODE, 
        (i == SW_D_INDEX || i == SW_UNUSED_INDEX) ? i + 2 : i, 
        BUTTON_STATE_IMMEDIATE, 
        ButtonEvent[PageNo][i][BTN_MSG], 
        ButtonEvent[PageNo][i][BTN_OPT]);
    }
    else
    {
      DisableButtonAction(
        IDLE_MODE, 
        (i == SW_D_INDEX || i == SW_UNUSED_INDEX) ? i + 2 : i, 
        BUTTON_STATE_IMMEDIATE);
    }
  }
  
  if (PageNo == NormalPage)
  {
    DisableButtonAction(IDLE_MODE, SW_A_INDEX, BUTTON_STATE_IMMEDIATE);
    EnableButtonAction(IDLE_MODE, SW_A_INDEX, BUTTON_STATE_PRESSED);
  }
  else
  {
    DisableButtonAction(IDLE_MODE, SW_A_INDEX, BUTTON_STATE_PRESSED);
  }
}

/* the default is for all simple button presses to be sent to the phone */
static void DefaultApplicationAndNotificationButtonConfiguration(void)
{
  /*
   * this will configure the pull switch even though it does not exist
   * on the watch
   */
  unsigned char index = 0;
  for(index = 0; index < NUMBER_OF_BUTTONS; index ++)
  {
    if ( index == SW_UNUSED_INDEX ) index ++;
    unsigned char m;
    for (m = 1; m < 4; m ++)
      DefineButtonAction(m, index, BUTTON_STATE_PRESSED, ButtonEventMsg, NO_MSG_OPTIONS);
  }
}

static void ToggleSecondsHandler(unsigned char Options)
{
  nvDisplaySeconds = !nvDisplaySeconds;

  if ( Options == TOGGLE_SECONDS_OPTIONS_UPDATE_IDLE )
  {
    IdleUpdateHandler(DATE_TIME_ONLY);
  }
}

static void DrawConnectionScreen()
{
//  unsigned char const* pSwash;
//
//  switch (CurrentPage[PAGE_TYPE_IDLE])
//  {
//  case RadioOnWithPairingInfoPage:
//    pSwash = pBootPageConnectionSwash;
//    break;
//  case RadioOnWithoutPairingInfoPage:
//    pSwash = pBootPagePairingSwash;
//    break;
//  case BluetoothOffPage:
//    pSwash = pBootPageBluetoothOffSwash;
//    break;
//  default:
//    pSwash = pBootPageUnknownSwash;
//    break;
//  }
//
//  FillMyBuffer(WATCH_DRAWN_IDLE_BUFFER_ROWS, PHONE_IDLE_BUFFER_ROWS, 0x00);
//  CopyRowsIntoMyBuffer(pSwash, WATCH_DRAWN_IDLE_BUFFER_ROWS + 1, 32);
//
//  /* local bluetooth address */
//  gRow = 65;
//  gColumn = 0;
//  gBitColumnMask = BIT4;
//  SetFont(MetaWatch7);
//  WriteFontString(GetLocalBluetoothAddressString());
//
//  /* add the firmware version */
//  gRow = 75;
//  gColumn = 0;
//  gBitColumnMask = BIT4;
//  DrawVersionInfo(10);
//  SendMyBufferToLcd(WATCH_DRAWN_IDLE_BUFFER_ROWS, PHONE_IDLE_BUFFER_ROWS);
}

static void DrawMenu1(void)
{
  unsigned char const * pIcon;

  if ( QueryConnectionState() == Initializing )
  {
    pIcon = pPairableInitIcon;
  }
  else
  {
    pIcon = QueryDiscoverable() ? pPairableIcon : pUnpairableIcon;
  }

  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

  /***************************************************************************/

  if ( QueryConnectionState() == Initializing )
  {
    pIcon = pBluetoothInitIcon;
  }
  else
  {
    pIcon = QueryBluetoothOn() ? pBluetoothOnIcon : pBluetoothOffIcon;
  }

  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

  pIcon = QueryLinkAlarmEnable() ? pLinkAlarmOnIcon : pLinkAlarmOffIcon;

  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_B_E_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
}

static void DrawMenu2(void)
{
  /* top button is always soft reset */
  CopyColumnsIntoMyBuffer(pResetButtonIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

  unsigned char const *pIcon = (RstPin() == RST_PIN_ENABLED) ? pRstPinIcon : pNmiPinIcon;

  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

  if ( QueryConnectionState() == Initializing )
  {
    pIcon = pSspInitIcon;
  }
  else
  {
    pIcon = QuerySecureSimplePairingEnabled() ? pSspEnabledIcon : pSspDisabledIcon;
  }

  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_B_E_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

}

static void DrawMenu3(void)
{
  unsigned char const * pIcon;

  if ( QueryInvertClock() )
  {
    pIcon = pClockInvertedMenuIcon;
  } else {
    pIcon = pNormalDisplayMenuIcon;
  }

  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

  pIcon = QueryAccelerometerState() ? pEnableAccelMenuIcon : pDisableAccelMenuIcon;
  
  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

#if 0
  /* shipping mode was removed for now */
  CopyColumnsIntoMyBuffer(pShippingModeIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
#endif

  pIcon = nvDisplaySeconds ? pSecondsOnMenuIcon : pSecondsOffMenuIcon;

  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_B_E_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
}

static void DrawCommonMenuIcons(void)
{
  CopyColumnsIntoMyBuffer(pNextIcon,
                          BUTTON_ICON_B_E_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

  CopyColumnsIntoMyBuffer(pLedIcon,
                          BUTTON_ICON_C_D_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

  CopyColumnsIntoMyBuffer(pExitIcon,
                          BUTTON_ICON_C_D_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
}

static void WatchStatusScreenHandler(void)
{
  StopDisplayTimer();

  /*
   * Add Status Icons
   */
  unsigned char const * pIcon;

  if ( QueryBluetoothOn() )
  {
    pIcon = pBluetoothOnStatusScreenIcon;
  }
  else
  {
    pIcon = pBluetoothOffStatusScreenIcon;
  }

  FillMyBuffer(STARTING_ROW, NUM_LCD_ROWS, 0x00);
  CopyColumnsIntoMyBuffer(pIcon,
                          0,
                          STATUS_ICON_SIZE_IN_ROWS,
                          LEFT_STATUS_ICON_COLUMN,
                          STATUS_ICON_SIZE_IN_COLUMNS);

  if ( QueryPhoneConnected() )
  {
    pIcon = pPhoneConnectedStatusScreenIcon;
  }
  else
  {
    pIcon = pPhoneDisconnectedStatusScreenIcon;
  }

  CopyColumnsIntoMyBuffer(pIcon,
                          0,
                          STATUS_ICON_SIZE_IN_ROWS,
                          CENTER_STATUS_ICON_COLUMN,
                          STATUS_ICON_SIZE_IN_COLUMNS);

  unsigned int bV = ReadBatterySenseAverage();

  if ( QueryBatteryCharging() )
  {
    pIcon = pBatteryChargingStatusScreenIcon;
  }
  else
  {
    if ( bV > 4000 )
    {
      pIcon = pBatteryFullStatusScreenIcon;
    }
    else if ( bV < 3500 )
    {
      pIcon = pBatteryLowStatusScreenIcon;
    }
    else
    {
      pIcon = pBatteryMediumStatusScreenIcon;
    }
  }

  CopyColumnsIntoMyBuffer(pIcon,
                          0,
                          STATUS_ICON_SIZE_IN_ROWS,
                          RIGHT_STATUS_ICON_COLUMN,
                          STATUS_ICON_SIZE_IN_COLUMNS);

  /* display battery voltage */
  unsigned char msd = 0;

  gRow = 27+2;
  gColumn = 8;
  gBitColumnMask = BIT6;
  SetFont(MetaWatch7);


  msd = bV / 1000;
  bV = bV % 1000;
  WriteFontCharacter(msd+'0');
  WriteFontCharacter('.');

  msd = bV / 100;
  bV = bV % 100;
  WriteFontCharacter(msd+'0');

  msd = bV / 10;
  bV = bV % 10;
  WriteFontCharacter(msd+'0');
  WriteFontCharacter(bV+'0');

  /*
   * Add Wavy line
   */
  gRow += 12;
  CopyRowsIntoMyBuffer(pWavyLine,gRow,NUMBER_OF_ROWS_IN_WAVY_LINE);

  /*
   * Add details
   */

  /* add MAC address */
  gRow += NUMBER_OF_ROWS_IN_WAVY_LINE+2;
  gColumn = 0;
  gBitColumnMask = BIT4;
  WriteFontString(GetLocalBluetoothAddressString());

  /* add the firmware version */
  gRow += 12;
  gColumn = 0;
  gBitColumnMask = BIT4;
  DrawVersionInfo(12);

  /* display entire buffer */
  SendMyBufferToLcd(STARTING_ROW, NUM_LCD_ROWS);
  
  PageType = PAGE_TYPE_INFO;
  CurrentPage[PageType] = WatchStatusPage;
  ConfigureIdleUserInterfaceButtons();

  /* refresh the status page once a minute */
  SetupOneSecondTimer(DisplayTimerId,
                      ONE_SECOND*60,
                      NO_REPEAT,
                      DISPLAY_QINDEX,
                      WatchStatusMsg,
                      NO_MSG_OPTIONS);

  StartOneSecondTimer(DisplayTimerId);

}

static void DrawVersionInfo(unsigned char RowHeight)
{
  WriteFontString("App ");
  WriteFontString(VERSION_STRING);
  WriteFontString(" Msp430 ");
  WriteFontCharacter(GetMsp430HardwareRevision());

  /* stack version */
  gRow += RowHeight;
  gColumn = 0;
  gBitColumnMask = BIT4;
  tVersion Version = GetWrapperVersion();
  WriteFontString("Stk ");
  WriteFontString(Version.pSwVer);
  WriteFontString(" ");
  WriteFontString(Version.pBtVer);
  WriteFontString(" ");
  WriteFontString(Version.pHwVer);
}

/* the bar code should remain displayed until the button is pressed again
 * or another mode is started
 */
static void BarCodeHandler(tMessage* pMsg)
{
  StopDisplayTimer();

  FillMyBuffer(STARTING_ROW, NUM_LCD_ROWS, 0x00);
  CopyRowsIntoMyBuffer(pBarCodeImage, BAR_CODE_START_ROW, BAR_CODE_ROWS);
  SendMyBufferToLcd(STARTING_ROW, NUM_LCD_ROWS);

  PageType = PAGE_TYPE_INFO;
  CurrentPage[PageType] = QrCodePage;
  ConfigureIdleUserInterfaceButtons();
}

static void ListPairedDevicesHandler(void)
{
  StopDisplayTimer();
  
  /* clearn screen */
  FillMyBuffer(STARTING_ROW, NUM_LCD_ROWS, 0x00);

  tString BluetoothAddress[12+1];
  tString BluetoothName[12+1];

  gRow = 4;
  gColumn = 0;
  SetFont(MetaWatch7);

  unsigned char i;
  for( i = 0; i < 3; i++)
  {
    QueryLinkKeys(i, BluetoothAddress, BluetoothName, 12);

    gColumn = 0;
    gBitColumnMask = BIT4;
    WriteFontString(BluetoothName);
    gRow += 12;

    gColumn = 0;
    gBitColumnMask = BIT4;
    WriteFontString(BluetoothAddress);
    gRow += 12+5;
  }

  SendMyBufferToLcd(STARTING_ROW, NUM_LCD_ROWS);

  PageType = PAGE_TYPE_INFO;
  CurrentPage[PageType] = ListPairedDevicesPage;
  ConfigureIdleUserInterfaceButtons();
}

/* change the parameter but don't save it into flash */
static void ConfigureDisplayHandler(tMessage* pMsg)
{
  switch (pMsg->Options)
  {
  case CONFIGURE_DISPLAY_OPTION_DONT_DISPLAY_SECONDS:
    nvDisplaySeconds = 0x00;
    break;
  case CONFIGURE_DISPLAY_OPTION_DISPLAY_SECONDS:
    nvDisplaySeconds = 0x01;
    break;
  case CONFIGURE_DISPLAY_OPTION_DONT_INVERT_DISPLAY:
    if(QueryInvertDisplay() == NORMAL_DISPLAY) 
    {
      nvIdleBufferInvert = (nvIdleBufferInvert & 0x02) | 0x00;
      UpdateDisplayHandler(IDLE_FULL_UPDATE);
    }
    break;
  case CONFIGURE_DISPLAY_OPTION_INVERT_DISPLAY:
     if(QueryInvertDisplay() == INVERT_DISPLAY) 
    {
      nvIdleBufferInvert = (nvIdleBufferInvert & 0x02) | 0x01;
      UpdateDisplayHandler(IDLE_FULL_UPDATE);
    }
    break;
  }
}

static void ConfigureIdleBufferSizeHandler(tMessage* pMsg)
{
  nvIdleBufferConfig = pMsg->pBuffer[0] & IDLE_BUFFER_CONFIG_MASK;
  //if ( nvIdleBufferConfig == WATCH_CONTROLS_TOP ) IdleUpdateHandler();
}

static void ModifyTimeHandler(tMessage* pMsg)
{
  int time;
  switch (pMsg->Options)
  {
  case MODIFY_TIME_INCREMENT_HOUR:
    /*! todo - make these functions */
    time = RTCHOUR;
    time++; if ( time == 24 ) time = 0;
    RTCHOUR = time;
    break;
  case MODIFY_TIME_INCREMENT_MINUTE:
    time = RTCMIN;
    time++; if ( time == 60 ) time = 0;
    RTCMIN = time;
    break;
  case MODIFY_TIME_INCREMENT_DOW:
    /* modify the day of the week (not the day of the month) */
    time = RTCDOW;
    time++; if ( time == 7 ) time = 0;
    RTCDOW = time;
    break;
  }

  /* now redraw the screen */
  IdleUpdateHandler(DATE_TIME_ONLY);
}

unsigned char GetIdleBufferConfiguration(void)
{
  return nvIdleBufferConfig;
}

static void InitMyBuffer(void)
{
  int row;
  int col;

  // Clear the display buffer.  Step through the rows
  for(row = STARTING_ROW; row < NUM_LCD_ROWS; row++)
  {
    // clear a horizontal line
    for(col = 0; col < NUM_LCD_COL_BYTES; col++)
    {
      pMyBuffer[row].Row = row+FIRST_LCD_LINE_OFFSET;
      pMyBuffer[row].Data[col] = 0x00;
      pMyBuffer[row].Dummy = 0x00;
    }
  }
}


static void FillMyBuffer(unsigned char StartingRow,
                         unsigned char NumberOfRows,
                         unsigned char FillValue)
{
  int row = StartingRow;
  int col;

  // Clear the display buffer.  Step through the rows
  for( ; row < NUM_LCD_ROWS && row < StartingRow+NumberOfRows; row++ )
  {
    // clear a horizontal line
    for(col = 0; col < NUM_LCD_COL_BYTES; col++)
    {
      pMyBuffer[row].Data[col] = FillValue;
    }
  }
}

static void SendMyBufferToLcd(unsigned char StartingRow, unsigned char NumberOfRows)
{
  int row = StartingRow;
  int col;

  /*
   * flip the bits before sending to LCD task because it will
   * dma this portion of the screen
  */
  if ( QueryInvertDisplay() == NORMAL_DISPLAY )
  {
    for( ; row < NUM_LCD_ROWS && row < StartingRow+NumberOfRows; row++)
    {
      for(col = 0; col < NUM_LCD_COL_BYTES; col++)
      {
        pMyBuffer[row].Data[col] = ~(pMyBuffer[row].Data[col]);
      }
    }
  }
  tLcdLine *pStartLcdLine = &pMyBuffer[StartingRow];
  UpdateMyDisplay((unsigned char*)pStartLcdLine, NumberOfRows);
}

static void CopyRowsIntoMyBuffer(unsigned char const* pImage,
                                 unsigned char StartingRow,
                                 unsigned char NumberOfRows)
{

  unsigned char DestRow = StartingRow;
  unsigned char SourceRow = 0;
  unsigned char col = 0;

  while ( DestRow < NUM_LCD_ROWS && SourceRow < NumberOfRows )
  {
    for(col = 0; col < NUM_LCD_COL_BYTES; col++)
    {
      pMyBuffer[DestRow].Data[col] = pImage[SourceRow*NUM_LCD_COL_BYTES+col];
    }
    DestRow ++;
    SourceRow ++;
  }
}

static void CopyColumnsIntoMyBuffer(unsigned char const* pImage,
                                    unsigned char StartingRow,
                                    unsigned char NumberOfRows,
                                    unsigned char StartingColumn,
                                    unsigned char NumberOfColumns)
{
  unsigned char DestRow = StartingRow;
  unsigned char RowCounter = 0;
  unsigned char DestColumn = StartingColumn;
  unsigned char ColumnCounter = 0;
  unsigned int SourceIndex = 0;

  /* copy rows into display buffer */
  while ( DestRow < NUM_LCD_ROWS && RowCounter < NumberOfRows )
  {
    DestColumn = StartingColumn;
    ColumnCounter = 0;
    while ( DestColumn < NUM_LCD_COL_BYTES && ColumnCounter < NumberOfColumns )
    {
      pMyBuffer[DestRow].Data[DestColumn] = pImage[SourceIndex];

      DestColumn ++;
      ColumnCounter ++;
      SourceIndex ++;
    }

    DestRow ++;
    RowCounter ++;
  }
}

static void DrawStatusIconCross(unsigned char bool)
{
	if ( !bool )
	{
		WriteFontCharacter(STATUS_ICON_CROSS);
	}
}

static void DrawLine( int x0, int y0, int x1, int y1)
{
    int steep = abs(y1 - y0) > abs(x1 - x0);
	int temp = 0;
    if (steep) {
    	temp = x0;
    	x0 = y0;
    	y0 = temp;

    	temp = x1;
    	x1 = y1;
    	y1 = temp;
    }
    if (x0 > x1) {
    	temp = x0;
    	x0 = x1;
    	x1 = temp;

    	temp = y0;
    	y0 = y1;
    	y1 = temp;
    }
    int deltax = x1 - x0;
    int deltay = abs(y1 - y0);
    int error = deltax / 2;
    int ystep;
    int x = x0;
    int y = y0;
    if (y0 < y1) {
    	ystep = 1;
    } else {
    	ystep = -1;
    }
	unsigned char bits[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
    for (; x <= x1; x++) {
        if (steep) {
        	int col = y/8;
        	int bit = bits[y%8];
        	pMyBuffer[x].Data[col] |= bit;
        } else {
        	int col = x/8;
        	int bit = bits[x%8];
        	pMyBuffer[y].Data[col] |= bit;
        }
        error = error - deltay;
        if (error < 0) {
            y = y + ystep;
            error = error + deltax;
        }
    }
}

const unsigned char bits[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
static void DrawTick(int x, int y, int w, int h) {
	int y0 = y;
	for (; y0<(y+h); y0++) {
		int x0 = x;
		for (; x0<(x+w); x0++) {
			int col = x0/8;
			unsigned char bit = bits[x0%8];
			pMyBuffer[y0].Data[col] |= bit;
		}
	}
}

static float sin(int angle) {
	int x = angle%360;
	int y = x%90;

	if (x <  90) return  sine_table[y];
	if (x < 180) return  sine_table[90-y];
	if (x < 270) return -sine_table[y];
				 return -sine_table[90-y];
}

static float cos(int angle) {
	return sin(angle+90);
}

static int RotatePoint(int x, int y, int angle) {
	return x * cos(angle) + y * sin(angle);
}

static void DrawHand(int x, int y, int tOffset, int lOffset, int bOffset, int rOffset, int angle) {
	int xLeft = RotatePoint(x+lOffset, y, angle);
	int yLeft = RotatePoint(y, x+lOffset, angle);

	int xTop = RotatePoint(x, y+tOffset, angle);
	int yTop = RotatePoint(y+tOffset, x, angle);

	int xRight = RotatePoint(x+rOffset, y, angle);
	int yRight = RotatePoint(y, x+rOffset, angle);

	int xBottom = RotatePoint(x, y+bOffset, angle);
	int yBottom = RotatePoint(y+bOffset, x, angle);

	DrawLine(xLeft,yLeft,xTop,yTop);
	DrawLine(xTop,yTop,xRight,yRight);
	DrawLine(xRight,yRight,xBottom,yBottom);
	DrawLine(xBottom,yBottom,xLeft,yLeft);
}

static void DrawAnalogueTime(unsigned char OnceConnected) {
	FillMyBuffer(0, 96, 0x00);
	DrawTick(47, 10, 4, 3); //tick for 12
	DrawTick(0, 47, 8, 4); //tick for 9
	DrawTick(88, 47, 8, 4); //tick for 3
	//no tick for 6 - overwritten by widget row

	int hour = RTCHOUR;
    int min = RTCMIN;

    hour %= 12; //convert to 12-hour display for analogue
	int hourAngle = (360.0/(12*60))*((hour*60)+min);
	DrawHand(48, 48, -20, -5, -5, 5, hourAngle); //hour hand

	int minAngle = (360/60) * min;
	DrawHand(48, 48, -35, -3, -3, 3, minAngle); //minute hand

	SendMyBufferToLcd(0, 96);
}

static void DrawDateTime(unsigned char OnceConnected)
{
	if (nvDisplaySeconds) {
		DrawAnalogueTime(OnceConnected);
		return;
	}

  unsigned char msd;
  unsigned char lsd;

  /* display hour */
  int Hour = RTCHOUR;

  /* if required convert to twelve hour format */
  if ( GetTimeFormat() == TWELVE_HOUR )
  {
    Hour %= 12;
    if (Hour == 0) Hour = 12;
  }
  
  msd = Hour / 10;
  lsd = Hour % 10;

  int actualDrawnBufferRows = WATCH_DRAWN_IDLE_BUFFER_ROWS;
  if (!QueryPhoneConnected()) actualDrawnBufferRows = 96;

  // clean date&time area
  FillMyBuffer(STARTING_ROW, actualDrawnBufferRows, 0x00);

//  if ( DisplayDisconnectWarning && (!QueryPhoneConnected()) )
//  {
//    CopyColumnsIntoMyBuffer(pPhoneDisconnectedIdlePageIcon,
//                            10,
//                            IDLE_PAGE_ICON_SIZE_IN_ROWS,
//                            1,
//                            IDLE_PAGE_ICON_SIZE_IN_COLS);
//
//
//    SetFont(MetaWatch16);
//
//    gColumn = 3;
//    gBitColumnMask = BIT4;
//    gRow = 11;
//    WriteFontString("Link Lost");
//
//  }
//  else
//  {

    gRow = 10;
    if ( nvDisplaySeconds )
    {
      gColumn = 0;
      gBitColumnMask = BIT6;
    }
    else
    {
      gColumn = 1;
      gBitColumnMask = BIT2;
    }
    SetFont(MetaWatchTallTime);

    /* if first digit is zero then leave location blank */
    if ( msd == 0 && GetTimeFormat() == TWELVE_HOUR )
    {
      WriteFontCharacter(TIME_CHARACTER_SPACE_INDEX);
    }
    else
    {
      WriteFontCharacter(msd);
    }

    WriteFontCharacter(lsd);

    WriteFontCharacter(TIME_CHARACTER_COLON_INDEX);

    /* display minutes */
    int Minutes = RTCMIN;
    msd = Minutes / 10;
    lsd = Minutes % 10;
    WriteFontCharacter(msd);
    WriteFontCharacter(lsd);

    if ( nvDisplaySeconds )
    {
      int Seconds = RTCSEC;
      msd = Seconds / 10;
      lsd = Seconds % 10;

      SetFont(MetaWatchSeconds);

      int mask = gBitColumnMask;
      gRow = 10;
      gColumn = 10;
      WriteFontCharacter(msd);
      gRow = 19;
      gColumn = 10;
      gBitColumnMask = mask;
      WriteFontCharacter(lsd);

    }

    if ( GetTimeFormat() == TWELVE_HOUR ) DisplayAmPm();

//  }

  SetFont(StatusIcons);
  gRow = 2;

  if ( OnceConnected )
  {
    char bluetooth = QueryBluetoothOn();
    char connected = QueryPhoneConnected();

    if ( (!bluetooth) || (bluetooth&&connected) ) {
      gColumn = 8;
      gBitColumnMask = BIT5;
    }
    else
    {
      gColumn = 8;
      gBitColumnMask = BIT1;
    }

    DrawStatusIconCross( bluetooth );
    WriteFontCharacter(STATUS_ICON_BLUETOOTH);

    if (bluetooth) {
      AdvanceBitColumnMask(1);
      DrawStatusIconCross( connected );
      WriteFontCharacter(STATUS_ICON_PHONE);
    }
  }

	gColumn = 10;
	gBitColumnMask = BIT0;
	if ( QueryBatteryCharging() )
	{
		WriteFontCharacter(STATUS_ICON_SPARK);
	}

	unsigned int bV = ReadBatterySenseAverage();

	gColumn = 10;
	gBitColumnMask = BIT6;

	if ( bV < 3500 )
	{
	  WriteFontCharacter(STATUS_ICON_BATTERY_EMPTY);
	}
	else if ( bV > 4000 )
	{
		WriteFontCharacter(STATUS_ICON_BATTERY_FULL);
	}
	else
	{
		WriteFontCharacter(STATUS_ICON_BATTERY_HALF);
	}

  DisplayDate();

  //if no connection, display connection warning prominently *full time*
  if (!QueryPhoneConnected()) {
//    CopyColumnsIntoMyBuffer(pPhoneDisconnectedIdlePageIcon, 87, IDLE_PAGE_ICON_SIZE_IN_ROWS, 1, IDLE_PAGE_ICON_SIZE_IN_COLS);
    SetFont(MetaWatch16);

    gRow = 72;
    gColumn = 2;
	gBitColumnMask = BIT6;
    WriteFontString("Link Lost");
  }

  // Invert the clock (because it looks good!)
  if ( QueryInvertClock() )
    {
    int row=0;
    int col=0;
    for( ; row < NUM_LCD_ROWS && row < actualDrawnBufferRows; row++)
    {
      for(col = 0; col < NUM_LCD_COL_BYTES; col++)
      {
        pMyBuffer[row].Data[col] = ~(pMyBuffer[row].Data[col]);
      }
    }
  }

  SendMyBufferToLcd(STARTING_ROW, actualDrawnBufferRows );
}

static void DisplayAmPm(void)
{
  int Hour = RTCHOUR;
  unsigned char const *pIcon = ( Hour >= 12 ) ? Pm : Am;
  WriteIcon4w10h(pIcon,16,0);
}

static void DisplayDate(void)
{
  gBitColumnMask = BIT2;
  gRow = 2;
  gColumn = 0;
  SetFont(MetaWatch5);
  WriteFontString((tString *)DaysOfTheWeek[GetLanguage()][RTCDOW]);

  if ( OnceConnected() )
  {
	WriteFontCharacter(' ');

  int First;
  int Second;

  /* determine if month or day is displayed first */
  if ( GetDateFormat() == MONTH_FIRST )
  {
    First = RTCMON;
    Second = RTCDAY;
  }
  else
  {
    First = RTCDAY;
    Second = RTCMON;
  }

  WriteFontCharacter(First/10+'0');
  WriteFontCharacter(First%10+'0');
  WriteFontCharacter('.');
  WriteFontCharacter(Second/10+'0');
  WriteFontCharacter(Second%10+'0');

  WriteFontCharacter('.');

  int year = RTCYEAR;
  WriteFontCharacter(year/1000+'0');
  year %= 1000;
  WriteFontCharacter(year/100+'0');
  year %= 100;
  WriteFontCharacter(year/10+'0');
  year %= 10;
  WriteFontCharacter(year+'0');

  }
}

/* these items are 4w by 10h */
static void WriteIcon4w10h(unsigned char const * pIcon,
                           unsigned char RowOffset,
                           unsigned char ColumnOffset)
{

  /* copy digit into correct position */
  unsigned char RowNumber;
  unsigned char Column;

  for ( Column = 0; Column < 4; Column++ )
  {
    for ( RowNumber = 0; RowNumber < 10; RowNumber++ )
    {
      // RM: Changed to |= to stop the icon overwriting the first time digit
      pMyBuffer[RowNumber+RowOffset].Data[Column+ColumnOffset] |=
        pIcon[RowNumber+(Column*10)];
    }
  }
}

unsigned char* GetTemplatePointer(unsigned char TemplateSelect)
{
  return NULL;
}

const unsigned char pBarCodeImage[BAR_CODE_ROWS * NUM_LCD_COL_BYTES] =
{
  0x00,0x00,0x00,0xFC,0xFF,0xFC,0xCF,0xFF,0x0F,0x00,0x00,0x00,
  0x00,0x00,0x00,0xFC,0xFF,0xFC,0xCF,0xFF,0x0F,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0C,0xC0,0xF0,0xC0,0x00,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0C,0xC0,0xF0,0xC0,0x00,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0xCC,0xCF,0xCC,0xCC,0xFC,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0xCC,0xCF,0xCC,0xCC,0xFC,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0xCC,0xCF,0x3C,0xC0,0xFC,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0xCC,0xCF,0x3C,0xC0,0xFC,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0xCC,0xCF,0xFC,0xCF,0xFC,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0xCC,0xCF,0xFC,0xCF,0xFC,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0C,0xC0,0x00,0xCF,0x00,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0C,0xC0,0x00,0xCF,0x00,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0xFC,0xFF,0xCC,0xCC,0xFF,0x0F,0x00,0x00,0x00,
  0x00,0x00,0x00,0xFC,0xFF,0xCC,0xCC,0xFF,0x0F,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0xF0,0x0C,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0xF0,0x0C,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0xFC,0xC3,0xCC,0x3F,0xFC,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0xFC,0xC3,0xCC,0x3F,0xFC,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0xF0,0x33,0x0C,0xFF,0x0C,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0xF0,0x33,0x0C,0xFF,0x0C,0x0C,0x00,0x00,0x00,
  0x00,0x00,0x00,0xFC,0xFC,0xF0,0xCF,0xF0,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0xFC,0xFC,0xF0,0xCF,0xF0,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x30,0xF3,0x03,0x33,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x30,0xF3,0x03,0x33,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0C,0xF3,0x0C,0x00,0x03,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0C,0xF3,0x0C,0x00,0x03,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0xFC,0xFF,0x03,0x03,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0xFC,0xFF,0x03,0x03,0x00,0x00,0x00,
  0x00,0x00,0x00,0xFC,0xFF,0x30,0xCC,0x30,0x0F,0x00,0x00,0x00,
  0x00,0x00,0x00,0xFC,0xFF,0x30,0xCC,0x30,0x0F,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0C,0xC0,0xF0,0x33,0x3F,0x0F,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0C,0xC0,0xF0,0x33,0x3F,0x0F,0x00,0x00,0x00,
  0x00,0x00,0x00,0xCC,0xCF,0x30,0x30,0xCC,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0xCC,0xCF,0x30,0x30,0xCC,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0xCC,0xCF,0xCC,0xCF,0xC0,0x03,0x00,0x00,0x00,
  0x00,0x00,0x00,0xCC,0xCF,0xCC,0xCF,0xC0,0x03,0x00,0x00,0x00,
  0x00,0x00,0x00,0xCC,0xCF,0xFC,0x33,0xF3,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0xCC,0xCF,0xFC,0x33,0xF3,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0C,0xC0,0x3C,0xCF,0xC3,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0C,0xC0,0x3C,0xCF,0xC3,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0xFC,0xFF,0x3C,0x03,0xF3,0x03,0x00,0x00,0x00,
  0x00,0x00,0x00,0xFC,0xFF,0x3C,0x03,0xF3,0x03,0x00,0x00,0x00
};

const unsigned char pMetaWatchSplash[SPLASH_ROWS * NUM_LCD_COL_BYTES] =
{
  0x00,0x00,0x00,0x00,0x30,0x60,0x80,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x30,0x60,0xC0,0x01,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x70,0x70,0xC0,0x01,0xE0,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x70,0xF0,0x40,0xE1,0xFF,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0xD8,0xD8,0x60,0x63,0xE0,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0xD8,0xD8,0x60,0x63,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0xC8,0x58,0x34,0x26,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x8C,0x0D,0x36,0x36,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0E,0x8C,0x0D,0x36,0x36,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0xFE,0x0F,0x05,0x1E,0x1C,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0E,0x00,0x07,0x1C,0x1C,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x07,0x0C,0x18,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x02,0x0C,0x18,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x30,0x18,0xFC,0xFC,0x70,0x04,0x00,0x31,0xFC,0xE1,0x83,0x40,
  0x30,0x18,0xFC,0xFC,0x70,0x04,0x02,0x31,0x20,0x18,0x8C,0x40,
  0x70,0x1C,0x0C,0x30,0x70,0x08,0x82,0x30,0x20,0x04,0x88,0x40,
  0x78,0x3C,0x0C,0x30,0xD8,0x08,0x85,0x48,0x20,0x04,0x80,0x40,
  0xD8,0x36,0x0C,0x30,0xD8,0x08,0x85,0x48,0x20,0x02,0x80,0x40,
  0xD8,0x36,0xFC,0x30,0x8C,0x91,0x48,0xCC,0x20,0x02,0x80,0x7F,
  0xDC,0x76,0xFC,0x30,0x8C,0x91,0x48,0x84,0x20,0x02,0x80,0x40,
  0x8C,0x63,0x0C,0x30,0xFC,0x91,0x48,0x84,0x20,0x02,0x80,0x40,
  0x8C,0x63,0x0C,0x30,0xFE,0xA3,0x28,0xFE,0x21,0x04,0x80,0x40,
  0x86,0xC3,0x0C,0x30,0x06,0xA3,0x28,0x02,0x21,0x04,0x88,0x40,
  0x06,0xC1,0xFC,0x30,0x03,0x46,0x10,0x01,0x22,0x18,0x8C,0x40,
  0x06,0xC1,0xFC,0x30,0x03,0x46,0x10,0x01,0x22,0xE0,0x83,0x40
};

const unsigned char Am[10*4] =
{
  0x00,0x00,0x9C,0xA2,0xA2,0xA2,0xBE,0xA2,0xA2,0x00,
  0x00,0x00,0x08,0x0D,0x0A,0x08,0x08,0x08,0x08,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

const unsigned char Pm[10*4] =
{
  0x00,0x00,0x9E,0xA2,0xA2,0x9E,0x82,0x82,0x82,0x00,
  0x00,0x00,0x08,0x0D,0x0A,0x08,0x08,0x08,0x08,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
/*
const unsigned char DaysOfWeek[7][10*4] =
{
0x00,0x00,0x9C,0xA2,0x82,0x9C,0xA0,0xA2,0x1C,0x00,
0x00,0x00,0x28,0x68,0xA8,0x28,0x28,0x28,0x27,0x00,
0x00,0x00,0x02,0x02,0x02,0x03,0x02,0x02,0x02,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x22,0xB6,0xAA,0xA2,0xA2,0xA2,0x22,0x00,
0x00,0x00,0x27,0x68,0xA8,0x28,0x28,0x28,0x27,0x00,
0x00,0x00,0x02,0x02,0x02,0x03,0x02,0x02,0x02,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0xBE,0x88,0x88,0x88,0x88,0x88,0x08,0x00,
0x00,0x00,0xE8,0x28,0x28,0xE8,0x28,0x28,0xE7,0x00,
0x00,0x00,0x03,0x00,0x00,0x01,0x00,0x00,0x03,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0xA2,0xA2,0xAA,0xAA,0xAA,0xAA,0x94,0x00,
0x00,0x00,0xEF,0x20,0x20,0x27,0x20,0x20,0xEF,0x00,
0x00,0x00,0x01,0x02,0x02,0x02,0x02,0x02,0x01,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0xBE,0x88,0x88,0x88,0x88,0x88,0x88,0x00,
0x00,0x00,0x28,0x28,0x28,0x2F,0x28,0x28,0xC8,0x00,
0x00,0x00,0x7A,0x8A,0x8A,0x7A,0x4A,0x8A,0x89,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0xBE,0x82,0x82,0x9E,0x82,0x82,0x82,0x00,
0x00,0x00,0xC7,0x88,0x88,0x87,0x84,0x88,0xC8,0x00,
0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x01,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x1C,0xA2,0x82,0x9C,0xA0,0xA2,0x9C,0x00,
0x00,0x00,0xE7,0x88,0x88,0x88,0x8F,0x88,0x88,0x00,
0x00,0x00,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
*/

/******************************************************************************/

void InitializeIdleBufferConfig(void)
{
  nvIdleBufferConfig = WATCH_CONTROLS_TOP;
  OsalNvItemInit(NVID_IDLE_BUFFER_CONFIGURATION,
                 sizeof(nvIdleBufferConfig),
                 &nvIdleBufferConfig);
}

void InitializeIdleBufferInvert(void)
{
  nvIdleBufferInvert = 0;
  OsalNvItemInit(NVID_IDLE_BUFFER_INVERT,
                 sizeof(nvIdleBufferInvert),
                 &nvIdleBufferInvert);
}

void InitializeDisplaySeconds(void)
{
  nvDisplaySeconds = 0;
  OsalNvItemInit(NVID_DISPLAY_SECONDS,
                 sizeof(nvDisplaySeconds),
                 &nvDisplaySeconds);
}

#if 0
static void SaveIdleBufferConfig(void)
{
  osal_nv_write(NVID_IDLE_BUFFER_CONFIGURATION,
                NV_ZERO_OFFSET,
                sizeof(nvIdleBufferConfig),
                &nvIdleBufferConfig);
}
#endif

static void SaveIdleBufferInvert(void)
{
  OsalNvWrite(NVID_IDLE_BUFFER_INVERT,
              NV_ZERO_OFFSET,
              sizeof(nvIdleBufferInvert),
              &nvIdleBufferInvert);
}

static void SaveDisplaySeconds(void)
{
  OsalNvWrite(NVID_DISPLAY_SECONDS,
              NV_ZERO_OFFSET,
              sizeof(nvDisplaySeconds),
              &nvDisplaySeconds);
}

unsigned char QueryDisplaySeconds(void)
{
  return nvDisplaySeconds;
}

unsigned char QueryInvertDisplay(void)
{
  return nvIdleBufferInvert & 0x01;
}

unsigned char QueryInvertClock(void)
{
  return (nvIdleBufferInvert & 0x02) >> 1;
}

/******************************************************************************/

static unsigned int CharacterMask;
static unsigned char CharacterRows;
static unsigned char CharacterWidth;
static unsigned int bitmap[MAX_FONT_ROWS];

static void AdvanceBitColumnMask(unsigned int pixels)
{
  int i=0;
  for(i = 0; i < pixels; i++)
  {
    gBitColumnMask = gBitColumnMask << 1;
    if ( gBitColumnMask == 0 )
    {
      gBitColumnMask = BIT0;
      gColumn++;
    }
  }
}

/* fonts can be up to 16 bits wide */
static void WriteFontCharacter(unsigned char Character)
{
  CharacterMask = BIT0;
  CharacterRows = GetCharacterHeight();
  CharacterWidth = GetCharacterWidth(Character);
  GetCharacterBitmap(Character,(unsigned int*)&bitmap);

  if ( gRow + CharacterRows > NUM_LCD_ROWS )
  {
    PrintString("Not enough rows to display character\r\n");
    return;
  }

  /* do things bit by bit */
  unsigned char i;
  unsigned char row;

  for (i = 0 ; i < CharacterWidth && gColumn < NUM_LCD_COL_BYTES; i++ )
  {
  	for(row = 0; row < CharacterRows; row++)
    {
      if ( (CharacterMask & bitmap[row]) != 0 )
      {
        pMyBuffer[gRow+row].Data[gColumn] |= gBitColumnMask;
      }
    }

    /* the shift direction seems backwards... */
    CharacterMask = CharacterMask << 1;
    AdvanceBitColumnMask(1);
  }

  /* add spacing between characters */
  AdvanceBitColumnMask(GetFontSpacing());
}

void WriteFontString(tString *pString)
{
  unsigned char i = 0;

  while (pString[i] != 0 && gColumn < NUM_LCD_COL_BYTES)
  {
    WriteFontCharacter(pString[i++]);
  }
}

unsigned char QueryButtonMode(void)
{
  return CurrentMode;
}

unsigned char LcdRtcUpdateHandlerIsr(void)
{
  unsigned char ExitLpm = 0;
  unsigned int RtcSeconds = RTCSEC;

  if ( RtcUpdateEnable )
  {
    /* send a message every second or once a minute */
    if (QueryDisplaySeconds() || lastMin != RTCMIN)
    {
      lastMin = RTCMIN;
      tMessage Msg;
      SetupMessage(&Msg, IdleUpdate, DATE_TIME_ONLY);
      SendMessageToQueueFromIsr(DISPLAY_QINDEX, &Msg);
      ExitLpm = 1;
    }
  }

  return ExitLpm;
}
