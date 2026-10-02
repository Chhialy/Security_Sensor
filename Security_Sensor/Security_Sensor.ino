#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

const int R_LED_PIN = 18;
const int G_LED_PIN = 19;
const int B_LED_PIN = 23;

const int PIR_PIN = 27;
const int BUZZER_PIN = 26;

const bool LED_ACTIVE_LOW = false;

const char* WIFI_NAME = "Chhialy";
const char* WIFI_PASSWORD = "lol120503";

const char* MQTT_SERVER = "thingsboard.cloud";
const int MQTT_PORT = 1883;
const char* MQTT_TOKEN = "96raUXiv8Xvm7c5PdieI";

const char* TELEMETRY_TOPIC = "v1/devices/me/telemetry";
const char* RPC_TOPIC = "v1/devices/me/rpc/request/+";

bool armed = false;

int motion_count = 0;
bool motion_event = false;

// Alarm/motion state
bool alarm_active = false;
bool muted = false;

// Time when motion was detected
unsigned long motion_start_time = 0;

// Prevent repeated motion detection
unsigned long last_motion_time = 0;

const unsigned long MOTION_DEBOUNCE = 3000;

// How long the red LED blinks after motion
const unsigned long ALARM_DURATION = 5000;

// Buzzer duration
const unsigned long BUZZER_DURATION = 2000;

// LED blink interval
const unsigned long BLINK_INTERVAL = 250;

// WIFI / MQTT
WiFiClient espClient;
PubSubClient mqtt_client(espClient);

// FREERTOS TASKS
TaskHandle_t wifi_task_handle = NULL;
TaskHandle_t mqtt_task_handle = NULL;
TaskHandle_t publish_task_handle = NULL;
TaskHandle_t process_task_handle = NULL;
TaskHandle_t sensor_task_handle = NULL;

QueueHandle_t payload_queue = NULL;

// LED FUNCTIONS
void writeLED(int pin, bool on)
{
  if (LED_ACTIVE_LOW)
  {
    digitalWrite(pin, on ? LOW : HIGH);
  }
  else
  {
    digitalWrite(pin, on ? HIGH : LOW);
  }
}

// Turn all LEDs off
void turnLEDOff()
{
  writeLED(R_LED_PIN, false);
  writeLED(G_LED_PIN, false);
  writeLED(B_LED_PIN, false);
}

// Green = system disarmed
void setGreenLED()
{
  writeLED(R_LED_PIN, false);
  writeLED(G_LED_PIN, true);
  writeLED(B_LED_PIN, false);
}

// Red = system armed
void setRedLED()
{
  writeLED(R_LED_PIN, true);
  writeLED(G_LED_PIN, false);
  writeLED(B_LED_PIN, false);
}

// White = count reset
void setWhiteLED()
{
  writeLED(R_LED_PIN, true);
  writeLED(G_LED_PIN, true);
  writeLED(B_LED_PIN, true);
}

// BUZZER FUNCTIONS: Active buzzer- simple ON/OFF
void buzzerOn()
{
  digitalWrite(BUZZER_PIN, HIGH);
}


void buzzerOff()
{
  digitalWrite(BUZZER_PIN, LOW);
}

// MQTT CALLBACK
void mqtt_callback(char* topic, byte* payload, unsigned int length)
{
  // Limit payload size to prevent buffer overflow
  if (length >= 100)
  {
    Serial.println("MQTT message too long");
    return;
  }

  char rx_payload[100];

  memcpy(rx_payload, payload, length);
  rx_payload[length] = '\0';

  // Put message into queue
  if (payload_queue != NULL)
  {
    xQueueSend(payload_queue, rx_payload, pdMS_TO_TICKS(150));
  }
}

// WIFI TASK
void wifi_task_fn(void *arg)
{
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);

  while (true)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println("wifi connected");

      if (mqtt_task_handle != NULL)
      {
        vTaskResume(mqtt_task_handle);
      }
    }
    else
    {
      Serial.println("wifi disconnected");

      if (mqtt_task_handle != NULL)
      {
        vTaskSuspend(mqtt_task_handle);
      }

      if (publish_task_handle != NULL)
      {
        vTaskSuspend(publish_task_handle);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// MQTT TASK
void mqtt_task_fn(void *arg)
{
  // Wait until Wi-Fi is connected
  vTaskSuspend(NULL);

  mqtt_client.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt_client.setCallback(mqtt_callback);

  while (true)
  {
    if (!mqtt_client.connected())
    {
      Serial.println("connecting to ThingsBoard...");

      bool status = mqtt_client.connect(
        "Light_Sensor",
        MQTT_TOKEN,
        ""
      );

      if (status)
      {
        Serial.println("mqtt connected");

        // Subscribe to RPC commands
        mqtt_client.subscribe(RPC_TOPIC);

        if (publish_task_handle != NULL)
        {
          vTaskResume(publish_task_handle);
        }
      }
      else
      {
        Serial.println("mqtt disconnected");

        if (publish_task_handle != NULL)
        {
          vTaskSuspend(publish_task_handle);
        }
      }
    }

    mqtt_client.loop();

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// PUBLISH TASK
void publish_task_fn(void *arg)
{
  // Wait until MQTT is connected
  vTaskSuspend(NULL);

  while (true)
  {
    if (mqtt_client.connected())
    {
      JsonDocument doc;

      doc["armed"] = armed;
      doc["motion_count"] = motion_count;
      doc["motion_event"] = motion_event;
      doc["alarm_active"] = alarm_active;
      doc["muted"] = muted;

      char json_payload[512];

      serializeJson(doc, json_payload);

      mqtt_client.publish(
        TELEMETRY_TOPIC,
        json_payload
      );

      Serial.print("published: ");
      Serial.println(json_payload);
    }

    // Publish every 3 seconds
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

// SENSOR TASK
void sensor_task_fn(void *arg)
{
  int last_pir = LOW;   

  while (true)
  {
    int pir = digitalRead(PIR_PIN);

    // Check for NEW motion (LOW -> HIGH only)
    if (
      armed &&
      !alarm_active &&            // ignore while an alarm is running
      pir == HIGH &&
      last_pir == LOW &&          // rising edge only
      (millis() - last_motion_time > MOTION_DEBOUNCE)
    )
    {
      last_motion_time = millis();

      motion_count++;
      motion_event = true;
      alarm_active = true;

      motion_start_time = millis();

      Serial.print("motion detected! count=");
      Serial.println(motion_count);

      setRedLED();

      if (!muted)
      {
        buzzerOn();
      }
    }

    last_pir = pir;               // updated every loop

    // Alarm timing
    if (alarm_active)
    {
      unsigned long elapsed =
        millis() - motion_start_time;

      // Buzzer stops after 2 seconds
      if (elapsed >= BUZZER_DURATION)
      {
        buzzerOff();
      }

      // Alarm ends after 5 seconds
      if (elapsed >= ALARM_DURATION)
      {
        alarm_active = false;
        motion_event = false;

        buzzerOff();

        if (armed)
        {
          setRedLED();
        }
        else
        {
          setGreenLED();
        }

        Serial.println("alarm ended");
      }
      else
      {
        // Blink red LED
        bool blinkState =
          ((elapsed / BLINK_INTERVAL) % 2) == 0;

        writeLED(R_LED_PIN, blinkState);
        writeLED(G_LED_PIN, false);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// PROCESS DASHBOARD COMMANDS
void process_task_fn(void *arg)
{
  char rx_cmd[100];

  while (true)
  {
    if (xQueueReceive(
          payload_queue,
          rx_cmd,
          portMAX_DELAY
        ) == pdTRUE)
    {
      Serial.print("get cmd: ");
      Serial.println(rx_cmd);

      JsonDocument cmd;

      DeserializationError error =
        deserializeJson(cmd, rx_cmd);

      if (error)
      {
        Serial.println("JSON error");
        continue;
      }

      const char* cmd_method =
        cmd["method"];

      if (cmd_method == NULL)
      {
        Serial.println("No method in command");
        continue;
      }

      // ARM
      if (
        strcmp(cmd_method, "arm") == 0 ||
        strcmp(cmd_method, "setArmed") == 0
      )
      {
        armed = true;
        muted = false;
        alarm_active = false;
        motion_event = false;

        buzzerOff();

        setRedLED();

        Serial.println("system armed");
      }

      // DISARM
      else if (
        strcmp(cmd_method, "disarm") == 0 ||
        strcmp(cmd_method, "setDisarmed") == 0
      )
      {
        armed = false;
        alarm_active = false;
        motion_event = false;

        buzzerOff();

        setGreenLED();

        Serial.println("system disarmed");
      }


      // RESET MOTION COUNT
      else if (
        strcmp(cmd_method, "reset") == 0 ||
        strcmp(cmd_method, "resetCount") == 0
      )
      {
        motion_count = 0;
        motion_event = false;
        alarm_active = false;

        buzzerOff();

        if (armed)
        {
          setRedLED();
        }
        else
        {
          setGreenLED();
        }

        Serial.println("count reset");
      }

      // CLEAR ALARM
      else if (
        strcmp(cmd_method, "clearAlarm") == 0
      )
      {
        alarm_active = false;
        motion_event = false;

        buzzerOff();

        if (armed)
        {
          setRedLED();
        }
        else
        {
          setGreenLED();
        }

        Serial.println("alarm cleared");
      }

      // MUTE
      else if (
        strcmp(cmd_method, "mute") == 0
      )
      {
        muted = true;

        buzzerOff();

        Serial.println("alarm muted");
      }


      // OLD LED COMMANDS
      else if (
        strcmp(cmd_method, "redled") == 0
      )
      {
        setRedLED();
        Serial.println("red LED");
      }


      else if (
        strcmp(cmd_method, "greenled") == 0
      )
      {
        setGreenLED();
        Serial.println("green LED");
      }

      // UNKNOWN COMMAND
      else
      {
        Serial.print("unknown command: ");
        Serial.println(cmd_method);
      }
    }
  }
}

// SETUP
void setup()
{
  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println(" Smart Security System");
  Serial.println(" Starting...");
  Serial.println("================================");

  // PIN SETUP
  pinMode(R_LED_PIN, OUTPUT);
  pinMode(G_LED_PIN, OUTPUT);
  pinMode(B_LED_PIN, OUTPUT);
  writeLED(B_LED_PIN, false);   // hold blue off

  pinMode(PIR_PIN, INPUT);

  pinMode(BUZZER_PIN, OUTPUT);

  // INITIAL STATE
  buzzerOff();

  // System starts DISARMED
  armed = false;

  // Green LED = disarmed
  setGreenLED();

  Serial.println("System: DISARMED");
  Serial.println("Green LED should be ON");

  // CREATE QUEUE
  payload_queue =
    xQueueCreate(20, 100);


  if (payload_queue == NULL)
  {
    Serial.println("ERROR: Queue creation failed!");
    return;
  }

  // CREATE TASKS
  xTaskCreate(
    process_task_fn,
    "process_task",
    5120,
    NULL,
    5,
    &process_task_handle
  );


  xTaskCreate(
    publish_task_fn,
    "publish_task",
    5120,
    NULL,
    5,
    &publish_task_handle
  );


  xTaskCreate(
    sensor_task_fn,
    "sensor_task",
    5120,
    NULL,
    4,
    &sensor_task_handle
  );


  xTaskCreate(
    mqtt_task_fn,
    "mqtt_task",
    5120,
    NULL,
    6,
    &mqtt_task_handle
  );


  xTaskCreate(
    wifi_task_fn,
    "wifi_task",
    4096,
    NULL,
    5,
    &wifi_task_handle
  );


  Serial.println("All tasks started");
}


// LOOP
void loop()
{
  // Everything is handled by FreeRTOS tasks.

  vTaskDelete(NULL);
}