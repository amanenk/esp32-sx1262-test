#include <Arduino.h>

#include <LoRaWan-Arduino.h>
#include <SPI.h>

#define SCHED_MAX_EVENT_DATA_SIZE APP_TIMER_SCHED_EVENT_DATA_SIZE /**< Maximum size of scheduler events. */
#define SCHED_QUEUE_SIZE 60                                       /**< Maximum number of events in the scheduler queue. */

#define LORAWAN_ADR_ON 1  /**< LoRaWAN Adaptive Data Rate enabled (the end-device should be static here). */
#define LORAWAN_ADR_OFF 0 /**< LoRaWAN Adaptive Data Rate disabled. */

#define LORAWAN_APP_DATA_BUFF_SIZE 64  /**< Size of the data to be transmitted. */
#define LORAWAN_APP_TX_DUTYCYCLE 30000 /**< Defines the application data transmission duty cycle. 10s, value in [ms]. */
#define APP_TX_DUTYCYCLE_RND 5000      /**< Defines a random delay for application data transmission duty cycle. 1s, value in [ms]. */
#define JOINREQ_NBTRIALS 3             /**< Number of trials for the join request. */

/**@brief Define activation procedure here
 * More information https://www.thethingsnetwork.org/forum/t/what-is-the-difference-between-otaa-and-abp-devices/2723
 * When set to 1 the application uses the Over-the-Air activation procedure
 * When set to 0 the application uses the Personalization activation procedure
 */
#define OVER_THE_AIR_ACTIVATION 1
#define LORAWAN_PUBLIC_NETWORK true

#define STATIC_DEVICE_EUI 1
#define LORAWAN_DEVICE_EUI {0x21, 0xA1, 0xCB, 0x0A, 0x4F, 0x9A, 0xA1, 0x54};
#define LORAWAN_APPLICATION_EUI {0xA0, 0x55, 0xA1, 0x4B, 0xCA, 0x25, 0x9C, 0xCB};
#define LORAWAN_APPLICATION_KEY {0x54, 0xA0, 0x9A, 0x4D, 0x0A, 0xC0, 0xA1, 0xC3, 0xCB, 0xBC, 0x00, 0xCA, 0xFB, 0xA1, 0x55, 0x11};

hw_config hwConfig;

// ESP32 - SX126x pin configuration
int PIN_LORA_RESET = 4;  // LORA RESET
int PIN_LORA_NSS = 17;   // LORA SPI CS
int PIN_LORA_SCLK = 18;  // LORA SPI CLK
int PIN_LORA_MISO = 19;  // LORA SPI MISO
int PIN_LORA_DIO_1 = 21; // LORA DIO_1
int PIN_LORA_BUSY = 22;  // LORA SPI BUSY
int PIN_LORA_MOSI = 23;  // LORA SPI MOSI

// Foward declaration
static void lorawan_has_joined_handler(void);
static void lorawan_rx_handler(lmh_app_data_t *app_data);
static void lorawan_confirm_class_handler(DeviceClass_t Class);
static void send_lora_frame(void);
static uint32_t timers_init(void);
uint8_t counter = 0;

// APP_TIMER_DEF(lora_tx_timer_id);                                              ///< LoRa tranfer timer instance.
TimerEvent_t appTimer;                                                        ///< LoRa tranfer timer instance.
static uint8_t m_lora_app_data_buffer[LORAWAN_APP_DATA_BUFF_SIZE];            ///< Lora user application data buffer.
static lmh_app_data_t m_lora_app_data = {m_lora_app_data_buffer, 0, 0, 0, 0}; ///< Lora user application data structure.

/**@brief Structure containing LoRaWan parameters, needed for lmh_init()
 */
static lmh_param_t lora_param_init = {LORAWAN_ADR_ON, LORAWAN_DEFAULT_DATARATE, LORAWAN_PUBLIC_NETWORK, JOINREQ_NBTRIALS, LORAWAN_DEFAULT_TX_POWER};

/**@brief Structure containing LoRaWan callback functions, needed for lmh_init()
*/
static lmh_callback_t lora_callbacks = {BoardGetBatteryLevel, BoardGetUniqueId, BoardGetRandomSeed,
                                        lorawan_rx_handler, lorawan_has_joined_handler, lorawan_confirm_class_handler};

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // Define the HW configuration between MCU and SX126x
  hwConfig.CHIP_TYPE = SX1262_CHIP;         // Example uses an eByte E22 module with an SX1262
  hwConfig.PIN_LORA_RESET = PIN_LORA_RESET; // LORA RESET
  hwConfig.PIN_LORA_NSS = PIN_LORA_NSS;     // LORA SPI CS
  hwConfig.PIN_LORA_SCLK = PIN_LORA_SCLK;   // LORA SPI CLK
  hwConfig.PIN_LORA_MISO = PIN_LORA_MISO;   // LORA SPI MISO
  hwConfig.PIN_LORA_DIO_1 = PIN_LORA_DIO_1; // LORA DIO_1
  hwConfig.PIN_LORA_BUSY = PIN_LORA_BUSY;   // LORA SPI BUSY
  hwConfig.PIN_LORA_MOSI = PIN_LORA_MOSI;   // LORA SPI MOSI
  hwConfig.USE_DIO2_ANT_SWITCH = true;      // Example uses an eByte E22 module which uses RXEN and TXEN pins as antenna control
  hwConfig.USE_DIO3_TCXO = true;            // Example uses an eByte E22 module which uses DIO3 to control oscillator voltage
  hwConfig.USE_DIO3_ANT_SWITCH = false;     // Only Insight ISP4520 module uses DIO3 as antenna control

  // Initialize Serial for debug output
  Serial.begin(115200);

  Serial.println("=====================================");
  Serial.println("SX126x LoRaWan test");
  Serial.println("=====================================");

  // Initialize Scheduler and timer
  uint32_t err_code = timers_init();
  if (err_code != 0)
  {
    Serial.printf("timers_init failed - %d\n", err_code);
  }

  // Initialize LoRa chip.
  err_code = lora_hardware_init(hwConfig);
  if (err_code != 0)
  {
    Serial.printf("lora_hardware_init failed - %d\n", err_code);
  }

  // Initialize LoRaWan
  err_code = lmh_init(&lora_callbacks, lora_param_init);
  if (err_code != 0)
  {
    Serial.printf("lmh_init failed - %d\n", err_code);
  }

  // Start Join procedure
  lmh_join();
}

void loop()
{
  // Handle Radio events
  Radio.IrqProcess();

  // We are on FreeRTOS, give other tasks a chance to run
  delay(10);
}

/**@brief LoRa function for handling HasJoined event.
 */
static void lorawan_has_joined_handler(void)
{
#if (OVER_THE_AIR_ACTIVATION != 0)
  Serial.println("Network Joined");
#else
  Serial.println("OVER_THE_AIR_ACTIVATION != 0");

#endif
  lmh_class_request(CLASS_A);
  TimerSetValue(&appTimer, LORAWAN_APP_TX_DUTYCYCLE);
  TimerStart(&appTimer);
  // app_timer_start(lora_tx_timer_id, APP_TIMER_TICKS(LORAWAN_APP_TX_DUTYCYCLE), NULL);
}

/**@brief Function for handling LoRaWan received data from Gateway
 *
 * @param[in] app_data  Pointer to rx data
 */
static void lorawan_rx_handler(lmh_app_data_t *app_data)
{
  Serial.printf("LoRa Packet received on port %d, size:%d, rssi:%d, snr:%d\n",
                app_data->port, app_data->buffsize, app_data->rssi, app_data->snr);

  switch (app_data->port)
  {
  case 3:
    // Port 3 switches the class
    if (app_data->buffsize == 1)
    {
      switch (app_data->buffer[0])
      {
      case 0:
        lmh_class_request(CLASS_A);
        break;

      case 1:
        lmh_class_request(CLASS_B);
        break;

      case 2:
        lmh_class_request(CLASS_C);
        break;

      default:
        break;
      }
    }
    break;

  case LORAWAN_APP_PORT:
    // YOUR_JOB: Take action on received data
    break;

  default:
    break;
  }
}

static void lorawan_confirm_class_handler(DeviceClass_t Class)
{
  Serial.printf("switch to class %c done\n", "ABC"[Class]);

  // Informs the server that switch has occurred ASAP
  m_lora_app_data.buffsize = 0;
  m_lora_app_data.port = LORAWAN_APP_PORT;
  lmh_send(&m_lora_app_data, LMH_UNCONFIRMED_MSG);
}

static void send_lora_frame(void)
{
  if (lmh_join_status_get() != LMH_SET)
  {
    //Not joined, try again later
    Serial.println("Did not join network, skip sending frame");
    return;
  }

  // if (digitalRead(PIN_LORA_BUSY) == 1)
  // {
  //   Serial.println("lora device is budy skipping sending frame");
  //   return;
  // }

  uint32_t i = 0;
  m_lora_app_data.port = LORAWAN_APP_PORT;
  m_lora_app_data.buffer[i++] = counter;
  m_lora_app_data.buffsize = i;

  counter++;
  if (counter >= 100)
  {
    counter = 0;
  }

  lmh_error_status error = lmh_send(&m_lora_app_data, LMH_UNCONFIRMED_MSG);
  if (error == LMH_SUCCESS)
  {
  }
  Serial.printf("lmh_send result %d\n", error);
}

/**@brief Function for handling a LoRa tx timer timeout event.
 */
static void tx_lora_periodic_handler(void)
{
  TimerSetValue(&appTimer, LORAWAN_APP_TX_DUTYCYCLE);
  TimerStart(&appTimer);
  Serial.println("Sending frame");
  send_lora_frame();
  TimerStart(&appTimer);
}

/**@brief Function for the Timer initialization.
 *
 * @details Initializes the timer module. This creates and starts application timers.
 */
static uint32_t timers_init(void)
{
  appTimer.timerNum = 3;
  appTimer.oneShot = true;
  TimerInit(&appTimer, tx_lora_periodic_handler);

  return 0;
}