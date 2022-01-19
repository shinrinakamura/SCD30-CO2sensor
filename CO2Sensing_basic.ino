/* 開発メモ
 * M5stackのボードマネージャとライブラリを使用します
 * adafruitのSCD30のライブラリを使用します
 * https://github.com/adafruit/Adafruit_SCD30
*/

#include <M5Stack.h>
#include <Adafruit_SCD30.h>
#include <WiFi.h>
#include <PubSubClient.h>

//#define MQTTMOD   //mqttを使用する場合コメントアウトを外す
#define DATANUM 20  //平均値のための配列のindex

//MQTTを使用するときは設定する必要がある-----------------------------------------
//ssidとパスワードの設定
const char *ssid = "your_ssid";
const char *password = "your_pass";
//Pub/Subの設定
const char* mqttHost = "server_address";    //ipアドレスかドメインで指定する
const int mqttPort = 1883;                  //通常は1883か8883
const char* topic = "topic_name";           // 送信するトピック名（変更）
//-------------------------------------------------------------------------

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

volatile int interrupt_flg = 0;
float temp[DATANUM];
float humi [DATANUM];
float co2[DATANUM];
float sum_temp, sum_humi, sum_co2;

int data_num = 0;           //配列のindex
//割り込み通知用のカウンター
volatile int timeCounter1 = 0;    //1秒ごとに上がっていく
volatile int MeasureDispay_flg = 1;

portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
//ハードウェアタイマーの準備
hw_timer_t *timer1 = NULL;

//割り込み時に呼ばれる割り込み関数
void IRAM_ATTR onTimer1(){
  //Increment the counter and set the time of ISR
  portENTER_CRITICAL_ISR(&timerMux);
  timeCounter1++;
  MeasureDispay_flg++;
  portEXIT_CRITICAL_ISR(&timerMux);
}

Adafruit_SCD30  scd30;
//センサーの初期化
void CO2sensorInit();
//Wi-Fiの接続
void connectWiFi();
//MQTTサーバーへの接続
void connectMqtt();
//データの送信
void MqttPublish(const char *payload);


void setup(void) {  
  M5.begin();
  Wire.begin(21,22);    //ボードマネージャでM5stackにした時とM5stackのライブラリを使用するときはピン番号を明確に指示する
  Serial.println("CO2 monitoriing start");
  CO2sensorInit();

#ifdef MQTTMOD
  connectWiFi();  
  connectMqtt();    //mQTTサーバーへの接続確認
#endif

  //タイマーのセット
  timer1 = timerBegin(0, 80, true);
  timerAttachInterrupt(timer1, &onTimer1, true);
  timerAlarmWrite(timer1, (1000000), true);   //1秒毎
  timerAlarmEnable(timer1);                   //タイマースタート

  //測定値の表示画面を作成する
  M5.Lcd.fillRect(0, 0, 320, 240, BLACK);
  M5.Lcd.fillRect(0, 0, 320, 40, BLUE);
  M5.Lcd.drawLine(0, 40, 320, 40, WHITE);
  M5.Lcd.drawString("CO2", 10, 45, 4);
  M5.Lcd.drawString("ppm", 250, 45, 4);  
  M5.Lcd.drawLine(0, 140, 320, 140, WHITE);
  M5.Lcd.drawString("temp", 10, 145, 4);
  M5.Lcd.drawString("c", 250, 145, 4);
  M5.Lcd.drawLine(0, 175, 320, 175, WHITE);
  M5.Lcd.drawString("humi", 10, 180, 4);
  M5.Lcd.drawString("%RH", 250, 180, 4);
  M5.Lcd.drawLine(0, 210, 320, 210, WHITE);
  M5.Lcd.setTextColor(WHITE  );
  M5.Lcd.drawString("CO2 sensor system", 40, 5, 4);
}

int n = 0;
void loop() {

  //センサーの測定
  if (scd30.dataReady()){   //データが準備できたら計測
    data_num += 1;
    Serial.println("Data available!");
    if (!scd30.read()){ Serial.println("Error reading sensor data"); return; }
    temp[data_num] = scd30.temperature;
    humi[data_num] = scd30.relative_humidity;
    co2[data_num] = scd30.CO2; 

    //測定値を表示する
    M5.Lcd.fillRect(80, 40, 160, 140, BLACK);   //表示部分のクリア
    M5.Lcd.fillRect(80, 140, 160, 175, BLACK);
    M5.Lcd.fillRect(80, 175, 160, 180, BLACK);
    char buf[8]; 
    dtostrf(co2[data_num], 5, 1, buf);          //floatを文字列に変換
    M5.Lcd.drawString(buf, 85, 45, 6);
    dtostrf(temp[data_num], 3, 1, buf);
    M5.Lcd.drawString(buf, 100, 145, 4);
    dtostrf(humi[data_num], 3, 1, buf);
    M5.Lcd.drawString(buf, 100, 180, 4);
    M5.Lcd.drawLine(0, 40, 320, 40, WHITE);
    M5.Lcd.drawLine(0, 140, 320, 140, WHITE);
    M5.Lcd.drawLine(0, 175, 320, 175, WHITE);
    M5.Lcd.drawLine(0, 210, 320, 210, WHITE);
    delay(100);
    //平均用に合計値を加算していく
    sum_temp += temp[data_num];
    sum_humi += humi[data_num];
    sum_co2 += co2[data_num];
    //デバッグ用の表示
    Serial.print(temp[data_num]);
    Serial.println(" degrees C");
    Serial.print("Relative Humidity: ");
    Serial.print(humi[data_num]);
    Serial.println(" %");
    Serial.print("CO2: ");
    Serial.print(co2[data_num], 3);
    Serial.println(" ppm");
    Serial.println("");
  } else {
    //Serial.println("No data");
  }
  
  //30秒ごとに平均値を作成して表示（送信）する
  if(timeCounter1 >= 30){
    timeCounter1 = 0;
    //平均値を計算
    float ave_temp, ave_humi, ave_co2;
    ave_temp = sum_temp / data_num;
    ave_humi = sum_humi / data_num;
    ave_co2 = sum_co2 / data_num;
    
    //配列をリセット
    data_num = 0;
    sum_temp = 0;
    sum_humi = 0;
    sum_co2 = 0; 

    //デバッグ用の表示
    Serial.print("Display and check the average value.\n");
    Serial.print("temp average : ");
    Serial.println(ave_temp);
    Serial.print("humi average : ");
    Serial.println(ave_humi);
    Serial.print("co2 average : ");
    Serial.println(ave_co2);
    Serial.println("");
#ifdef MQTTMOD      
    //mqttの送信
    if (n != 0){    //最初の３０秒間は値が安定しないので捨てる
       char payload[150];   //適当なサイズにしています
       sprintf(payload,"{\"temp\":%f,\"humi\":%f,\"co2\":%f}", ave_temp, ave_humi, ave_co2);
       Serial.println("publish message");
       MqttPublish(payload);
    }
#endif  
  n = 1;  
  }
}   //loopend


//センサーの初期化
void CO2sensorInit(){
  // Try to initialize!
  if (!scd30.begin()) {
    Serial.println("Failed to find SCD30 chip");
    while (1) { delay(10); }
  }
  Serial.println("SCD30 Found!");  
  Serial.print("Measurement Interval: "); 
  Serial.print(scd30.getMeasurementInterval()); 
  Serial.println(" seconds");
}

//通信関係の処理
//--------------------------------------------------------
//Wi-Fiを接続する
void connectWiFi(){
     
  WiFi.begin(ssid, password);
  Serial.print("WiFi connecting...");
  int i = 0 ;   //接続確認の時間
  while(WiFi.status() != WL_CONNECTED) {
  i += 1;
  Serial.print(".");
  delay(1000);
  //10秒間Wi-Fiが接続できないときは接続をやり直す
  if (i == 10){
    Serial.println("WiFi reset");
    connectWiFi();
      }
  }      
  Serial.print(" connected. ");
  Serial.println(WiFi.localIP());
}

//MQTTサーバーへの接続
void connectMqtt(){
  while(WiFi.status() != WL_CONNECTED) {
  Serial.print("WiFi is not connect");
  connectWiFi();
  }
 
  mqttClient.setServer(mqttHost, mqttPort);   //brokerサーバーに接続

  while( ! mqttClient.connected() ) {
    Serial.println("Connecting to MQTT...");
    //MacアドレスからクライアントIDを作成する
    String clientId = "ESP32-" +  getMacAddr();
    Serial.print("clientID : "); 
    Serial.println(clientId.c_str()); 
    //接続の確認
    //if ( mqttClient.connect(clientId.c_str(),mqtt_username, mqtt_password) ) {  //ユーザー認証を行う時はこちらを利用する
    if ( mqttClient.connect(clientId.c_str())) { 
      Serial.println("connected"); 
    }
  }
}

//Mqttの送信
void MqttPublish(const char *payload){

  //mqttの接続を確認
  while( ! mqttClient.connected() ) {
    Serial.println("Mqtt is not connect");
    connectMqtt();
  }
  mqttClient.publish(topic, payload);   //mqttの送信
  Serial.print("published ");
  Serial.println(payload);

}

// Macアドレスを文字列で取得する
String getMacAddr(){
  byte mac[6];
  char buf[50];
  WiFi.macAddress(mac);
  sprintf(buf, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}
