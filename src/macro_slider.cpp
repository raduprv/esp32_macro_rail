#include <Arduino.h>
#include <WiFi.h>
#include <BleKeyboard.h>
#include <HTTPClient.h>
#include <Preferences.h>
//#include <WiFiClient.h>


BleKeyboard bleKeyboard;

int disconnected=0;
int connected=0;
int callbacks_already_set=0;

#define SHUTTER_NONE 0
#define SHUTTER_BT_PHONE 1
#define SHUTTER_CANON_IR 2
#define SHUTTER_NIKON_IR 3
#define SHUTTER_PANASONIC_WIFI 4
#define SHUTTER_OLYMPUS_WIFI 5
#define SHUTTER_CANON_PTP_IP 6
#define SHUTTER_SONY_CAMERA_REMOTE_API 7
#define SHUTTER_NIKON_PTP_IP 8

typedef struct config_struct {  
  char ssid[32];      // your network SSID (name)
  char pass[32];   // your network password

  char camera_ip[32];
  char camera_ssid[32];
  char camera_pass[32];
  char esp32_ssid[32];
  char esp32_pass[32];

  int initial_delay=1000;
  int delay_after_shutter=2000;
  int delay_after_move=1000;
  int total_photos=20;
  int steps_to_move=2;
  int shutter_type=SHUTTER_BT_PHONE;
  char magic_number=7;
} config_struct;

config_struct config;

Preferences preferences;

#define MAIN_AP 0
#define CAMERA_AP 1

char final_string[4096];
char header_string[]="HTTP/1.1 200 OK\nContent-Type: text/html\nConnection: close\n\n<!DOCTYPE HTML>\n<html>";
char status_string[2048];
char ptp_read_buffer[1024];


WiFiServer server(80);
WiFiClient client;

char request_string[256];


//char config.camera_ssid[] = "E-M10MKII-P-BHKC69367";      // your network SSID (name)
//char config.camera_pass[] = "43452507";   // your network password



#define STEP_PIN 13
#define DIR_PIN 12
#define ENABLE_PIN 26
#define IR_LED_PIN 15
#define LIGHT_1_PIN 2
#define LIGHT_2_PIN 4

int light_1_state=0;
int light_2_state=0;

WiFiClient command_connection;
WiFiClient event_connection;
uint32_t sessionId;

#define InitRequestEnd 1
#define InitResponseEnd 0x10000

enum Type
{
  T_None,
  T_InitRequest,
  T_InitResponse,
  T_EventRequest,
  T_EventResponse,
  T_InitFail,
  T_CmdRequest,
  T_CmdResponse,
  T_Event,
  T_DataStart,
  T_Data,
  T_CancelTransaction,
  T_DataEnd,
};

enum Code
{
  C_GetPropDesc = 0x1014,
  C_GetProp = 0x1015,
  C_SetProp = 0x1016,
  C_GetAllPropsDesc = 0x9614,
  C_Open = 0x1002,
  C_Close = 0x1003,
  C_Mode = 0xD604
};

struct  __attribute__((packed)) ptpip_header
{
  uint32_t length;
  uint32_t type;
};

struct  __attribute__((packed)) init_request
{
  struct ptpip_header header;
  uint8_t guid[16];
  uint8_t name[16];//fixed size name because cba
  uint16_t end;
  
};

struct  __attribute__((packed)) init_response
{
  struct ptpip_header header;
  uint16_t session;
  uint8_t camera[16];
  uint32_t zerobytes;
  uint32_t end;
};

struct  __attribute__((packed)) event_request
{
  struct ptpip_header header;
  uint32_t code;
};

struct  __attribute__((packed)) event_response
{
  struct ptpip_header header;
};

struct  __attribute__((packed)) cmd_request
{
  struct ptpip_header header;
  uint32_t flag;
  uint16_t code;
  uint32_t transactionid;
};

struct  __attribute__((packed)) cmd_open_request
{
  struct cmd_request cmd;
  uint32_t param;
};

struct  __attribute__((packed)) cmd_set_param_16b
{
  struct cmd_request cmd;
  uint32_t prop;
  uint32_t value;
};

void print_hex_string(char* start, int len)
{
  int i;

  for(i=0;i<len;i++)
  {
    Serial.print(start[i],HEX);
    Serial.print(",");
  }
  Serial.println("");
}

int cptpipGetResponse(WiFiClient which_connection)
{

  int total_len;
  int bytes;
  int tries=0;

  while(!which_connection.available())
  {
    delay(100);
    tries++;

    if(tries>30)
    {
      Serial.println("Timeout in cptpipGetResponse");
      return 0;
    }
  }  

  bytes = which_connection.readBytes((uint8_t*)ptp_read_buffer, 8);//recv(socket, &header, sizeof(header), 0);

  if(bytes<=0)
  {
    Serial.println("Got no connection data in cptpipGetResponse... (0 or fewer bytes)");
    return 0;
  }


    if(bytes!=8)
      {
        Serial.print("!!!!!!! bytes!=8");
        Serial.println(bytes);
        return 0;
      }

    total_len=(uint32_t)ptp_read_buffer[0];

    Serial.print("We read ");
    Serial.println(bytes);
    Serial.print("size of response is ");
    Serial.println(total_len);
    
    if(total_len > 8)
    {
      int currentBytes;
      bytes = 0;
      while(bytes < total_len - 8)
      {
        //if(which_connection.available() && which_connection.connected())
        currentBytes = which_connection.readBytes((uint8_t*)ptp_read_buffer+8+bytes, total_len - 8 - bytes);//recv(socket, response+sizeof(header)+bytes, header.length - sizeof(header) - bytes, 0);
        if(currentBytes >= 0)
          {
            bytes += currentBytes;
            Serial.print("Bytes: ");
            Serial.println(bytes);
          }
      }
      print_hex_string(ptp_read_buffer,total_len);
    }
    else
    {
      Serial.println("Got header only...");
    }
  
  return 1;
}

bool cptpipInitEventSocket()
{
  bool result = false;
  
  struct event_request request;
  request.header.length = sizeof(request);
  request.header.type = T_EventRequest;
  request.code = sessionId;
  if(event_connection.write((uint8_t*)&request, request.header.length));//send(eventSocket, &request, request.header.length, 0) == request.header.length)
  {
    Serial.println("Calling cptpipGetResponse(event_connection)");
    if(cptpipGetResponse(event_connection))
    {
      struct ptpip_header *header = (struct ptpip_header *)ptp_read_buffer;
      if(header->type == T_EventResponse && header->length == sizeof(struct event_response))
      {
        result = true;
      }
    }
  }

  Serial.println("cptpipInitEventSocket DONE");
  return result;
}

bool cptpipOpenSession()
{
  bool result = false;
  
  struct cmd_open_request request;
  request.cmd.header.length = sizeof(request);
  request.cmd.header.type = T_CmdRequest;
  request.cmd.flag = 1;
  request.cmd.code = C_Open;
  request.cmd.transactionid = sessionId;
  request.param = 1;
  if(command_connection.write((uint8_t*)&request, request.cmd.header.length) == request.cmd.header.length)
  {
    if(cptpipGetResponse(command_connection))
    {
      struct ptpip_header *header = (struct ptpip_header *)ptp_read_buffer;
      if(header->type == T_CmdResponse)
      {
        uint16_t response=*((uint16_t*)ptp_read_buffer+4);
        if(response==0x2001)
        result = true;
      }
    }
  }
  return result;
}
int ptp_set_property_value(int property, int value)
{
  struct cmd_open_request request;

  request.cmd.header.length = sizeof(request);
  request.cmd.header.type = T_CmdRequest;
  request.cmd.flag = 2;
  request.cmd.code = C_SetProp;
  request.cmd.transactionid = sessionId;
  request.param = property;

  char data_buffer[20]={0x14,0,0,0,9,0,0,0,0x18,0,0,0,0x08,0,0,0,0,0,0,0};
  *((uint32_t*)data_buffer+2)=sessionId;
  char end_data_buffer[20]={0x14,00,00,00,0x0c,00,00,00,0x18,00,00,00,0x08,00,00,00,02,0xd1};
  *((uint32_t*)end_data_buffer+2)=sessionId;
  *((uint32_t*)end_data_buffer+4)=value;

  //18000000,0c000000,93000000,0c000000,b0d10000,08000000

   if(command_connection.write((uint8_t*)&request, sizeof(request)) != sizeof(request))
   {
    Serial.println("Failed to send data in canon_set_property_value");
    return 0;
   }

   if(command_connection.write((uint8_t*)&data_buffer, 20) != 20)
   {
    Serial.println("Failed to send data in set_property_value");
    return 0;
   }

   if(command_connection.write((uint8_t*)&end_data_buffer, 20) != 20)
   {
    Serial.println("Failed to send data in set_property_value");
    return 0;
   }

    if(cptpipGetResponse(command_connection))
    {      
      struct ptpip_header *header = (struct ptpip_header *)ptp_read_buffer;
      Serial.print("In set prop, response is ");
      uint16_t response=*((uint16_t*)ptp_read_buffer+4);
      Serial.println(response,HEX);  
    }
    else 
    {
      Serial.println("Hmm, no response for set prop");
      return 0;
    }   

  return 1;
}
uint16_t send_ptp_command_1_arg(uint16_t command, int arg)
{
  uint16_t response=0;

  if(!command_connection)
  {
    Serial.println("Command connection not even initialized....");
    return 0;    
  }

  if(!command_connection.connected())
  {
    Serial.println("Command connection not connected...");
    return 0;    
  }
  
  
  Serial.print("Command is ");
  Serial.println(command,HEX);

  struct cmd_open_request request;
  request.cmd.header.length = sizeof(request);
  request.cmd.header.type = T_CmdRequest;
  request.cmd.flag = 1;
  request.cmd.code = command;
  request.cmd.transactionid = sessionId;
  request.param = arg;

  if(command_connection.write((uint8_t*)&request, request.cmd.header.length) == request.cmd.header.length)
  {
    Serial.println(command,HEX);
    if(cptpipGetResponse(command_connection))
    {      
      struct ptpip_header *header = (struct ptpip_header *)ptp_read_buffer;
      Serial.print("In command, command is ");
      Serial.println(command,HEX);
      Serial.print("In command, header type is ");
      Serial.println(header->type,HEX);   

      Serial.print("In command, response is ");
      response=*((uint16_t*)ptp_read_buffer+4);
      Serial.println(response,HEX);  
    }
    else 
    {
      Serial.print("Hmm, no response for command ");
      Serial.println(command,HEX);
      delay(1000);
    }
  }
  else
  {
    Serial.println("Hmm, pula write....");
  }

  return response;
}

uint16_t send_ptp_command_no_arg(uint16_t command)
{  
  uint16_t response=send_ptp_command_1_arg(command,1);
  return response;
}

int canon_set_property_value(int property, int value)
{
  char operation_buffer[18]={0x12,00,00,00,06,00,00,00,02,00,00,00,0x10,0x91,00,00,00,00};
  *((uint32_t*)operation_buffer+4)=sessionId;
  char data_buffer[20]={0x14,0,0,0,9,0,0,0,0x18,0,0,0,0x0c,0,0,0,0,0,0,0};
  *((uint32_t*)data_buffer+2)=sessionId;
  char end_data_buffer[24]={0x18,00,00,00,0x0c,00,00,00,0x18,00,00,00,0x0c,00,00,00,02,0xd1,00,00,0x65,00,00,00};
  *((uint32_t*)end_data_buffer+2)=sessionId;
  *((uint32_t*)end_data_buffer+4)=property;
  *((uint32_t*)end_data_buffer+5)=value;

  //18000000,0c000000,93000000,0c000000,b0d10000,08000000

   if(command_connection.write((uint8_t*)&operation_buffer, 18) != 18)
   {
    Serial.println("Failed to send data in canon_set_property_value");
    return 0;
   }

   if(command_connection.write((uint8_t*)&data_buffer, 20) != 20)
   {
    Serial.println("Failed to send data in set_property_value");
    return 0;
   }

   if(command_connection.write((uint8_t*)&end_data_buffer, 24) != 24)
   {
    Serial.println("Failed to send data in set_property_value");
    return 0;
   }

    if(cptpipGetResponse(command_connection))
    {      
      struct ptpip_header *header = (struct ptpip_header *)ptp_read_buffer;
      Serial.print("In set prop, response is ");
      uint16_t response=*((uint16_t*)ptp_read_buffer+4);
      Serial.println(response,HEX);  
    }
    else 
    {
      Serial.println("Hmm, no response for set prop");
      return 0;
    }   

  return 1;
}



int connect_ptpip(const char* camera_ip)
{
  bool result = false;
  int i;

  command_connection.connect(camera_ip,15740);

  if(command_connection.connected())Serial.println("Connected to camera (command connection)");
  else
  {
    Serial.println("Couldn't connect to camera (command connection)");
    return 0;
  }
  
  //struct init_response response;
  //memset(&response, 0, sizeof(response));
  struct init_request request;
  memset(&request, 0, sizeof(request));
  request.header.length = sizeof(request);
  request.header.type = T_InitRequest;
  strcpy((char*)request.guid, "Penis");
  
  
  for(uint8_t i = 0; i < 7; ++i)
  {
    request.name[i*2] = 'i';
  }

  request.end = InitRequestEnd;

  if(command_connection.write((uint8_t*)&request, request.header.length) == request.header.length)
  //if(command_connection.write(test_buffer, sizeof(test_buffer)))
  {    
    if(cptpipGetResponse(command_connection))
    {
      Serial.println("Got response");

      struct ptpip_header *header = (struct ptpip_header *)ptp_read_buffer;      

      if(header->type == T_InitResponse)
      {
        Serial.println("Header type ok");
        struct init_response *initresponse = (struct init_response *)ptp_read_buffer;
        //if(initresponse->end == InitResponseEnd)
        {
          sessionId = initresponse->session;
          result = true;
          Serial.print("Session is ");
          Serial.println(sessionId);
        }
      }
    }
  }
  else Serial.println("command_connection.write failed...");

  if(!result)return -1;

  event_connection.connect(camera_ip,15740);

  if(event_connection.connected())Serial.println("Connected to camera (event connection)");
    else
    {
      Serial.println("Couldn't connect to camera (event connection)");
      return 0;
    }

  if(!cptpipInitEventSocket())
  {
    //cptpipUninit(*socket, *eventSocket, *sessionId);
    Serial.println("Init event socket failed...");
    return false;
  }

  Serial.println("Init event socket OK");

  if(!cptpipOpenSession())
  {
    //cptpipUninit(*socket, *eventSocket, *sessionId);
    Serial.println("Open Session failed...");
    return false;
  }  

  Serial.println("PTP IP all good...");

  return result;
}

void flush_events_buffer()
{
  char str[128];
  int bytes;
  int total_read=0;
  
  while(1)
  {
    bytes = event_connection.readBytes((uint8_t*)str, 100);
    total_read+=bytes;
    if(!bytes)break;
  }

  Serial.print("In flush events buffer, we read this many bytes:");
  Serial.println(total_read);

}

bool ptp_canon_shoot(int cur_shot)
{
  int i=0;
  Serial.println("Got ptp_canon_shoot()");

  if(!command_connection)
    {
      connect_ptpip(config.camera_ip);
      canon_set_property_value(0xd1b0,8);//shoot mode/live view
      canon_set_property_value(0xd11c,0x2);//destination to SD (rather than ram). Not needed for M3 but might be needed for others.
      send_ptp_command_no_arg(0x9114);//SetRemoteMode
      send_ptp_command_no_arg(0x9115);//SetEventMode
    }  
  else
  if(!command_connection.connected())
    {
      connect_ptpip(config.camera_ip);
      canon_set_property_value(0xd1b0,8);//shoot mode/live view
      canon_set_property_value(0xd11c,0x2);//destination to SD (rather than ram). Not needed for M3 but might be needed for others.
      send_ptp_command_no_arg(0x9114);//SetRemoteMode
      send_ptp_command_no_arg(0x9115);//SetEventMode
    }  



  while(send_ptp_command_no_arg(0x9128)==0x2019)//While StartImageCapture returns busy
  {
    delay(100);
    i++;
    if(i>30)
    {
      Serial.println("StartImageCapture still busy after 3 seconds, giving up...");
      break;
    }
  }

  send_ptp_command_no_arg(0x9129);//StopImageCapture 

  //if the events buffer gets full, bad things happen (disconnects)
  if(!(cur_shot%5))
  flush_events_buffer();
  
  Serial.println("Left ptp_canon_shoot()");

  return 0;
}


void save_config()
{
  /*
  Serial.println("Saving config");
  ESP.flashEraseSector(0x9000/4096);
  ESP.flashWrite(0x9000, (uint32_t*)&config, sizeof(config));
  */

  preferences.putString("ssid", config.ssid); 
  preferences.putString("pass", config.pass);
  preferences.putString("camera_ip", config.camera_ip);
  preferences.putString("camera_pass", config.camera_pass);
  preferences.putString("camera_ssid", config.camera_ssid);
  preferences.putString("esp32_pass", config.esp32_pass);
  preferences.putString("esp32_ssid", config.esp32_ssid);
  preferences.putInt("delay_af_move", config.delay_after_move);
  preferences.putInt("delay_af_st", config.delay_after_shutter);
  preferences.putInt("initial_delay", config.initial_delay);
  preferences.putInt("shutter_type", config.shutter_type);
  preferences.putInt("steps_to_move", config.steps_to_move);
  preferences.putInt("total_photos", config.total_photos);
  preferences.putChar("magic_number", 7);

}

void load_config()
{
  Serial.println("Loading config");  
  //ESP.flashRead(0x9000, (uint32_t*)&config, sizeof(config));
  String myString;
  char* str;

  myString=preferences.getString("ssid", "");
  str=(char*)myString.c_str();
  strcpy(config.ssid,str);

  myString=preferences.getString("pass","");
  str=(char*)myString.c_str();
  strcpy(config.pass,str);

  myString=preferences.getString("camera_ip","");
  str=(char*)myString.c_str();
  strcpy(config.camera_ip,str);

  myString=preferences.getString("camera_pass","");
  str=(char*)myString.c_str();
  strcpy(config.camera_pass,str);

  myString=preferences.getString("camera_ssid","");
  str=(char*)myString.c_str();
  strcpy(config.camera_ssid,str);

  myString=preferences.getString("esp32_pass","");
  str=(char*)myString.c_str();
  strcpy(config.esp32_pass,str);

  myString=preferences.getString("esp32_ssid","");
  str=(char*)myString.c_str();
  strcpy(config.esp32_ssid,str);

  config.delay_after_move=preferences.getInt("delay_af_move", 1000);

  config.delay_after_shutter=preferences.getInt("delay_af_st", 1000);
  config.initial_delay=preferences.getInt("initial_delay", 10000);
  config.shutter_type=preferences.getInt("shutter_type", 1);
  config.steps_to_move=preferences.getInt("steps_to_move", 5);
  config.total_photos=preferences.getInt("total_photos", 20);

}

void ConnectedToAP_Handler(WiFiEvent_t wifi_event, WiFiEventInfo_t wifi_info) {
  Serial.println("Connected To the AP");
  disconnected=0;
}
 

void GotIP_Handler(WiFiEvent_t wifi_event, WiFiEventInfo_t wifi_info) {
  Serial.print("Local ESP32 IP: ");
  Serial.println(WiFi.localIP());
  connected=1;

  //restart the server when the station changes
  //if(server.available)
  {
    server.end();
    server.begin();
  }

}

void Ap_Started(WiFiEvent_t wifi_event, WiFiEventInfo_t wifi_info) {
  Serial.println("SoftAP started");
  //Serial.println(WiFi.localIP());

  server.begin();
}

void WiFiStationDisconnected(WiFiEvent_t wifi_event, WiFiEventInfo_t wifi_info) {
  Serial.println("Disconnected");

  disconnected=1;
  //server.end();
}


void nikon_pulse_on(unsigned long duration) 
{
    unsigned long stop = micros() + duration;
    while(micros()<stop) 
    {
        digitalWrite(IR_LED_PIN, HIGH);
        delayMicroseconds(13);
        digitalWrite(IR_LED_PIN, LOW);
        delayMicroseconds(13);
    }
}

void nikon_ir_take_photo() 
{
  int i;
    for(i=0;i<2;i++) 
    {
        nikon_pulse_on(2000);
        delayMicroseconds(27850);
        nikon_pulse_on(390);
        delayMicroseconds(1580);
        nikon_pulse_on(410);
        delayMicroseconds(3580);
        nikon_pulse_on(400);
        delayMicroseconds(63200);
    }
}

void canon_ir_take_photo()
{
  int i,j;

  for(j=0;j<2;j++)
  {
	  for(i=0; i<16; i++)
	  {
		  digitalWrite(IR_LED_PIN, HIGH);
		  delayMicroseconds(15.24);
		  digitalWrite(IR_LED_PIN, LOW);
		  delayMicroseconds(15.24);
	  }

    delayMicroseconds(7330);
  }
	
}

void do_panasonic_pairing()
{
  HTTPClient http;
  char str[128];
  int http_return;

  sprintf(str,"http://%s%s",config.camera_ip,"/cam.cgi?mode=accctrl&type=req_acc&value=4D454930-0100-1000-8001-02FA000430C6&value2=MI%205");

  http.begin(str);
  http_return = http.GET();
  http.end();

  sprintf(str,"http://%s%s",config.camera_ip,"/cam.cgi?mode=setsetting&type=device_name&value=MI%205");
  http.begin(str);
  http_return = http.GET();
  http.end();

}

void panasonic_take_photo()
{
  HTTPClient http;
  char str[128];
  int http_return;

  sprintf(str,"http://%s%s",config.camera_ip,"/cam.cgi?mode=camcmd&value=recmode");
  
  http.begin(str);
  http_return = http.GET();
  http.end();
  sprintf(str,"http://%s%s",config.camera_ip,"/cam.cgi?mode=camcmd&value=capture");

  http.begin(str);
  http_return = http.GET();

  http.end();

  Serial.print("The return of the http request is ");
  Serial.println(http_return);

}

void disconnect_from_ap()
{
  
  if(!connected)return;

  WiFi.disconnect();

  while(!disconnected)
  {
    delay(50);
  }

  connected=0;
}

void olympus_take_photo()
{
  char str[128];
  int http_return;

  while(!connected)
  {
      delay(50);
  }
  

  //sprintf(str,"http://%s%s",config.camera_ip,"/cam.cgi?mode=camcmd&value=capture");

  HTTPClient http;
  http.begin("http://192.168.0.10/switch_cammode.cgi?mode=shutter");
  http_return = http.GET();
  http.end();

  delay(50);

  http.begin("http://192.168.0.10/exec_shutter.cgi?com=1st2ndpush");
  //http.begin("http://192.168.0.10/exec_shutter.cgi?com=2ndpush");
  http_return = http.GET();
  http.end();

  delay(50);

  http.begin("http://192.168.0.10/exec_shutter.cgi?com=2nd1strelease");
  //http.begin("http://192.168.0.10/exec_shutter.cgi?com=2ndrelease");
  http_return = http.GET();
  http.end();

  Serial.print("The return of the http request is ");
  Serial.println(http_return);
}

void move_forward(int steps) 
{
  int i;

  digitalWrite(ENABLE_PIN, LOW);
  delay(100);

  digitalWrite(DIR_PIN, HIGH);

  for(i=0;i<steps;i++)
    {
      digitalWrite(STEP_PIN, HIGH);
      delayMicroseconds(500);
      digitalWrite(STEP_PIN, LOW);
      delayMicroseconds(500);
    }
  delay(100);
  digitalWrite(ENABLE_PIN, HIGH);  
}

void move_backward(int steps) 
{
  int i;

  digitalWrite(ENABLE_PIN, LOW);

  digitalWrite(DIR_PIN, LOW);

  for(i=0;i<steps;i++)
    {
      digitalWrite(STEP_PIN, HIGH);
      delayMicroseconds(500);
      digitalWrite(STEP_PIN, LOW);
      delayMicroseconds(500);
    }

  digitalWrite(ENABLE_PIN, HIGH);  
}

void connect_to_main_ap()
{
  
  if(!config.ssid[0])
  {
    Serial.println("Can't connect to Wifi, ssid not set");  
    return;
  }

  if(!callbacks_already_set)
  {
    WiFi.onEvent(ConnectedToAP_Handler, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent(GotIP_Handler, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(WiFiStationDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);    
    callbacks_already_set=1;
  }
  WiFi.begin(config.ssid, config.pass);
  Serial.println("\nConnecting to main WiFi Network ..");    
  
}

void connect_to_camera_ap()
{

  WiFi.begin(config.camera_ssid, config.camera_pass);
  Serial.println("\nConnecting to camera WiFi");
}

void test_nikon()
{
  //this is a manual test/debug
  client.println("HTTP/1.1 200 OK\nContent-Type: text/html\nConnection: close\n\n<!DOCTYPE HTML>\n<html>\nYou are now being disconnected from the main AP. The connection will be restored after the sequence is complete. Once done, click <a href='/'>here</a> to come back to this page.</html>");
  client.stop();
  strcpy(config.camera_pass,"NikonCoolpix");
  strcpy(config.camera_ssid,"NikonS960040007605");
  disconnect_from_ap();
  connect_to_camera_ap();

  for(int i=0;i<50;i++)
  {
    if(connected)break;
    delay(100);
  }

  Serial.println("Attempting PTP connection");
  connect_ptpip("192.168.0.10");

  Serial.println("Sending shoot command");
  send_ptp_command_1_arg(0x9201	,0xfffffffe);//live view
  delay(100);
  ptp_set_property_value(0x5008,3500);//zoom
  delay(1000);
  ptp_set_property_value(0x5008,4600);//zoom
  ptp_set_property_value(0x5008,0);//zoom
  ptp_set_property_value(0x5008,1);//zoom
  send_ptp_command_1_arg(0x9207,0xffffffff);//Nikon capture
  delay(100);
  ptp_set_property_value(0x5006,0);//rbg gain

  send_ptp_command_1_arg(0x9207,0xffffffff);//Nikon capture

  Serial.println("Connecting back to main AP");
  disconnect_from_ap();
  connect_to_main_ap();  
}

bool ptp_nikon_shoot(int cur_shot)
{
  int i=0;
  Serial.println("Got ptp_nikon_shoot()");

  if(!command_connection)
    {
      connect_ptpip("192.168.0.10");
      send_ptp_command_1_arg(0x9201	,0xfffffffe);//live view    
    }  
  else
  if(!command_connection.connected())
    {
      connect_ptpip("192.168.0.10");
      send_ptp_command_1_arg(0x9201	,0xfffffffe);//live view
    }  

  send_ptp_command_1_arg(0x100e,0);//PTP capture

  //if the events buffer gets full, bad things happen (disconnects)
  if(!(cur_shot%5))
  flush_events_buffer();
  
  Serial.println("Left ptp_nikon_shoot()");

  return 0;
}


void do_shutter(int cur_shot)
{
  if(config.shutter_type==SHUTTER_BT_PHONE)
  bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);
  else
  if(config.shutter_type==SHUTTER_CANON_IR)
  canon_ir_take_photo();
  else
  if(config.shutter_type==SHUTTER_NIKON_IR)
  nikon_ir_take_photo();
  else
  if(config.shutter_type==SHUTTER_PANASONIC_WIFI)
  panasonic_take_photo();
  else
  if(config.shutter_type==SHUTTER_CANON_PTP_IP)
  ptp_canon_shoot(cur_shot);
}

void do_olympus_focus_bracketing()
{
  int i;

  disconnect_from_ap();
  connect_to_camera_ap();

    delay(config.initial_delay);
    for(i=0;i<config.total_photos;i++)
    {
      olympus_take_photo();
      delay(config.delay_after_shutter);
      move_forward(config.steps_to_move);
      delay(config.delay_after_move);
    }

  
  Serial.println("Connecting back to main AP");
  disconnect_from_ap();
  connect_to_main_ap();
}

// JSON structures from API documentation
char startRecMode[] = "{\"method\":\"startRecMode\",\"params\":[],\"id\":2,\"version\":\"1.0\"}"; // Go to Rec mode
char startLiveview[] = "{\"method\":\"startLiveview\",\"params\":[],\"id\":2,\"version\":\"1.0\"}"; // Go to live view
char setShootMode[] = "{\"method\":\"setShootMode\",\"params\":[\"still\"],\"id\":2,\"version\":\"1.0\"}"; // Set shot mode to "still"
char actTakePicture[] = "{\"method\":\"actTakePicture\",\"params\":[],\"id\":2,\"version\":\"1.0\"}"; // Take a picture
char setFocusMode[] = "{\"method\":\"setFocusMode\",\"params\":[\"AF-S\"],\"id\":2,\"version\":\"1.0\"}"; // Set focus mode to Single AF
char setAutoPowerOff[] = "{\"method\":\"setAutoPowerOff\",\"params\":[{\"autoPowerOff\":60}],\"id\":2,\"version\":\"1.0\"}"; // Set automatic power off after 60 seconds

void httpPost(char* j_request) {
  // Creates domain name
  char str[128];
  //String server_name = "http://" + config.camera_ip + ":" + "10000" + "/sony/camera";
  //sprintf(str,"http://%s:10000/sony/camera",config.camera_ip);
  sprintf(str,"http://192.168.122.1:8080/sony/camera");
  //sprintf(str,"http://192.168.122.1:10000/sony/camera");

  
  HTTPClient http;
  // Domain name with IP, port and URL
  http.begin(str);
  // Specify content-type
  http.addHeader("Content-Type", "application/json");
  // Send HTTP POST
  int http_response_code = http.POST(j_request);
  // Free resources
  Serial.print("HTTP Response code: ");
  Serial.println(http_response_code);  
  http.end();
}

void sony_shoot()
{  
  httpPost(actTakePicture);
  //delay(1000);
}

void do_sony_api_focus_bracketing()
{
  int i;

  disconnect_from_ap();
  connect_to_camera_ap();

  while(!connected)
  {
      delay(50);
  }

  httpPost(startRecMode);
  delay(2000);
  httpPost(setShootMode);
  delay(400);  

    delay(config.initial_delay);
    for(i=0;i<config.total_photos;i++)
    {
      sony_shoot();
      delay(config.delay_after_shutter);
      move_forward(config.steps_to_move);
      delay(config.delay_after_move);
    }

  
  Serial.println("Connecting back to main AP");
  disconnect_from_ap();
  connect_to_main_ap();
}

void do_nikon_ptp_bracketing()
{
  int i;

  disconnect_from_ap();
  connect_to_camera_ap();

  for(i=0;i<30;i++)
  {
    if(connected)
    break;  
    delay(100);
  }

  if(i==30)goto nikon_ptp_end;

  connect_ptpip("192.168.0.10");
  send_ptp_command_1_arg(0x9201	,0xfffffffe);//live view     


  delay(config.initial_delay);
  for(i=0;i<config.total_photos;i++)
  {
    ptp_nikon_shoot(i);
    delay(config.delay_after_shutter);
    move_forward(config.steps_to_move);
    delay(config.delay_after_move);
  }

  nikon_ptp_end:
  Serial.println("Connecting back to main AP");
  disconnect_from_ap();
  connect_to_main_ap();
}

void do_focus_bracketing()
{

    int i;

    client.println("HTTP/1.1 200 OK\nContent-Type: text/html\nConnection: close\n\n<!DOCTYPE HTML>\n<html>\nYou are now being disconnected from the main AP. The connection will be restored after the sequence is complete. Once done, click <a href='/'>here</a> to come back to this page.</html>");
    client.stop();

    if(config.shutter_type==SHUTTER_OLYMPUS_WIFI)
    {
      do_olympus_focus_bracketing();
      return;
    }

    if(config.shutter_type==SHUTTER_SONY_CAMERA_REMOTE_API)
    {
      do_sony_api_focus_bracketing();
      return;
    }

    if(config.shutter_type==SHUTTER_NIKON_PTP_IP)
    {
      do_nikon_ptp_bracketing();
      return;
    }

    delay(config.initial_delay);
    for(i=0;i<config.total_photos;i++)
    {
      do_shutter(i);
      delay(config.delay_after_shutter);
      move_forward(config.steps_to_move);
      delay(config.delay_after_move);
    }


}

void setup() 
{
  int config_magic_number;

  //WiFi.useStaticBuffers(true);  

  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(ENABLE_PIN, OUTPUT);
  pinMode(IR_LED_PIN, OUTPUT);
  pinMode(LIGHT_1_PIN, OUTPUT);
  pinMode(LIGHT_2_PIN, OUTPUT);
  

  digitalWrite(ENABLE_PIN, HIGH);//disable driver to not waste power
  digitalWrite(IR_LED_PIN, LOW);//disable IR LED to not waste power
  digitalWrite(LIGHT_1_PIN, LOW);//disable lights to not waste power
  digitalWrite(LIGHT_2_PIN, LOW);//disable lights to not waste power

  //Initialize serial and wait for port to open:
  Serial.begin(115200);

  preferences.begin("macro_slider", false);

  load_config();

  WiFi.onEvent(Ap_Started, ARDUINO_EVENT_WIFI_AP_START);
  WiFi.mode(WIFI_MODE_APSTA);
  //WiFi.setMinSecurity(WIFI_AUTH_WPA2_WPA3_PSK);	
  WiFi.softAP("esp32", "testtest");

  //ESP.flashRead(0x9000, (uint32_t*)&config_magic_number, 1);
  //if(config_magic_number==7)load_config();

  bleKeyboard.begin();
  connect_to_main_ap();

}

int get_int_in_get(char* uri,char* my_string)
{
    char str[20];
    char* start_of_string;
    int i;

    start_of_string=strstr(uri, my_string);

    if(!start_of_string)return 0;

    start_of_string+=strlen(my_string)+1;//skip over "="

    for(i=0;i<19;i++)
    {
        if(start_of_string[i]>='0' && start_of_string[i]<='9')
        {
            str[i]=start_of_string[i];
        }
        else break;
    }

    str[i]=0;

    return atoi(str);
}

void urldecode2(char *dst, const char *src)
{
        char a, b;
        while (*src) {
                if ((*src == '%') &&
                    ((a = src[1]) && (b = src[2])) &&
                    (isxdigit(a) && isxdigit(b))) {
                        if (a >= 'a')
                                a -= 'a'-'A';
                        if (a >= 'A')
                                a -= ('A' - 10);
                        else
                                a -= '0';
                        if (b >= 'a')
                                b -= 'a'-'A';
                        if (b >= 'A')
                                b -= ('A' - 10);
                        else
                                b -= '0';
                        *dst++ = 16*a+b;
                        src+=3;
                } else if (*src == '+') {
                        *dst++ = ' ';
                        src++;
                } else {
                        *dst++ = *src++;
                }
        }
        *dst++ = '\0';
}

int get_string_in_get(char* uri,char* my_string, char* returned_string, int max_len)
{
    char* start_of_string;
    int i,j;
    int uri_len=strlen(uri)-11;//no idea why -11 instead of -9 (to get rid of the  HTTP/1.1 at the end)
    int start_pos;

    start_of_string=strstr(uri, my_string);
    
    if(!start_of_string)return 0;

    start_of_string+=strlen(my_string)+1;//skip over "="

    start_pos=start_of_string-uri;

    for(i=0,j=0;i+start_pos<uri_len && j<max_len;i++,j++)
    {
        if(start_of_string[i]!='&')
        {
            returned_string[j]=start_of_string[i];
        }
        else break;
    }

    returned_string[i]=0;

    urldecode2(returned_string,returned_string);
    return 1;
}

void print_ssid_config_page()
{
  char str [1024];
  sprintf(str,"HTTP/1.1 200 OK\nContent-Type: text/html\nConnection: close\n\n<!DOCTYPE HTML>\n<html>\n"
    "<center><table><form action='/apply_net_stuff' method='get'>"
    "\n<tr><td colspan='2'>Configure your network SSID and Password</td><tr>"
    "<tr><td><label for='set_ssid'>SSID:</label></td><td><input type='text' name='set_ssid' value='%s'></td></tr>"
    "<tr><td><label for='set_pass'>Password:</label></td><td><input type='password' name='set_pass' value='%s'></td></tr>"
    "\n<tr><td colspan='2'><input type='submit' value='Apply'></form></td>"
    "\n<td><form action='/' method='get'><input type='submit' value='Back'></form></td>"
    "<tr></form></table></center>"
    "</html>",config.ssid,config.pass);
    client.println(str);
    client.stop();  
}

void print_camera_config()
{
  char str [2048];
  sprintf(str,"HTTP/1.1 200 OK\nContent-Type: text/html\nConnection: close\n\n<!DOCTYPE HTML>\n<html>\n"
    "<center><table><tr><td><form action='/apply_camera_stuff' method='get' enctype='text/plain'><label for='set_camera_ip'>Camera IP:</label></td><td><input type='text' name='set_camera_ip' value='%s'></td></tr>"
    "\n<tr><td><label for='set_camera_ssid'>Camera SSID:</label></td><td><input type='text' name='set_camera_ssid' value='%s'></td></tr>"
    "\n<tr><td><label for='set_camera_pass'>Camera Pass:</label></td><td><input type='text' name='set_camera_pass' value='%s'></td></tr>"
    "\n<tr><td><input type='submit' value='Set'></form></td>"
    "\n<td><form action='/' method='get'><input type='submit' value='Back'></form></td>"
    "</tr></table></center>"
    "<center><table>"
    "\n<td><form action='/bt_test' method='get'><input type='submit' value='Test Phone BT Shutter'></form></td>"
    "\n<td><form action='/canon_ir_test' method='get'><input type='submit' value='Test Canon IR'></form></td>"
    "\n<td><form action='/do_panasonic_pairing' method='get'><input type='submit' value='Pair Panasonic'></form></td>"
    "\n<td><form action='/panasonic_test' method='get'><input type='submit' value='Test Panasonic'></form></td>"
    "\n<td><form action='/oly_test' method='get'><input type='submit' value='Test Olympus'></form></td>"
    "\n<td><form action='/ptp_shoot' method='get'><input type='submit' value='Canon PTP Shoot'></form></td>"
    "\n<td><form action='/sony_test' method='get'><input type='submit' value='Test Sony'></form></td>"
    "\n<td><form action='/nikon_test' method='get'><input type='submit' value='Nikon PTP Shoot'></form></td>"
    "</center></table>"
    "</html>",config.camera_ip,config.camera_ssid,config.camera_pass);
    client.println(str);
    client.stop();   
}

void apply_net_stuff()
{
  preferences.putString("ssid", config.ssid); 
  preferences.putString("pass", config.pass);
  disconnect_from_ap();  
  connect_to_main_ap();
}

void apply_camera_stuff()
{
  preferences.putString("camera_ip", config.camera_ip);
  preferences.putString("camera_pass", config.camera_pass);
  preferences.putString("camera_ssid", config.camera_ssid);
  print_camera_config();
}

void loop() 
 {


  // listen for incoming clients
  
  client = server.available();
  
  if (client) 
  {
    int lines_no=0;
    int char_no=0;
    
    //Serial.println("new client");
    // an http request ends with a blank line
    boolean currentLineIsBlank = true;
    while (client.connected())
    {

      if (client.available())
      {
        char c = client.read();
        Serial.write(c);

        if(!lines_no && char_no<256)
          {
            request_string[char_no]=c;
            char_no++;
          }
        
        // if you've gotten to the end of the line (received a newline
        // character) and the line is blank, the http request has ended,
        // so you can send a reply
        //if(c == '\n' && currentLineIsBlank) 
        if(lines_no)
        {

          Serial.print("Request string is: ");
          Serial.println(request_string);
          //process the command
        if (strstr(request_string, "move_forward="))
        {
            int steps_to_move=get_int_in_get(request_string,(char*)"move_forward");
            move_forward(steps_to_move);
            
            Serial.print("Moving forward ");
            Serial.print(steps_to_move);
            Serial.println(" steps");
            //cerror(stream, filename, "400", "Ftp port changed","Ftp port changed");
        }
        else
        if (strstr(request_string, "move_backward="))
        {
            int steps_to_move=get_int_in_get(request_string,(char*)"move_backward");
            move_backward(steps_to_move);
            
            Serial.print("Moving backward ");
            Serial.print(steps_to_move);
            Serial.println(" steps");
            //cerror(stream, filename, "400", "Ftp port changed","Ftp port changed");
        }        
        else
        if (strstr(request_string, "bt_test"))
        {          
          Serial.println("Doing bt testing....");
          if(bleKeyboard.isConnected())Serial.print("Keyboard is connected");
          else Serial.print("Keyboard is NOT connected!!!");
          //bleKeyboard.print("Hello world");                    
          bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN );
          print_camera_config();
          break;          
        }
        else
        if (strstr(request_string, "canon_ir_test"))
        {
          canon_ir_take_photo();
          Serial.println("Testing IR LED...");
          print_camera_config();
          break;          
        }
        else
        if (strstr(request_string, "ptp_connect"))
        {
          connect_ptpip(config.camera_ip);
     
        }
        else
        if (strstr(request_string, "ptp_shoot"))
        {
          ptp_canon_shoot(5);
          print_camera_config();
          break;     
        } 
        else
        if (strstr(request_string, "do_panasonic_pairing"))
        {
          do_panasonic_pairing();
          print_camera_config();
          break;
        }                                     
        else
        if (strstr(request_string, "panasonic_test"))
        {
          panasonic_take_photo();
          Serial.println("Testing Panasonic...");
          print_camera_config();
          break;          
        }
        else
        if (strstr(request_string, "nikon_test"))
        {          
          Serial.println("Testing Nikon...");
          test_nikon();
          print_camera_config();
          break;          
        }          
        else
        if (strstr(request_string, "oly_test"))
        {
          client.println("HTTP/1.1 200 OK\nContent-Type: text/html\nConnection: close\n\n<!DOCTYPE HTML>\n<html>\nYou are now being disconnected from the main AP. The connection will be restored after the sequence is complete. Once done, click <a href='/'>here</a> to come back to this page.</html>");
          client.stop();
          Serial.println("Testing Olympus...");
          disconnect_from_ap();
          connect_to_camera_ap();
          olympus_take_photo();
          Serial.println("Connecting back to main AP");
          disconnect_from_ap();
          connect_to_main_ap();
          print_camera_config();
          break;          
        }
        else
        if (strstr(request_string, "sony_test"))
        {
          client.println("HTTP/1.1 200 OK\nContent-Type: text/html\nConnection: close\n\n<!DOCTYPE HTML>\n<html>\nYou are now being disconnected from the main AP. The connection will be restored after the sequence is complete. Once done, click <a href='/'>here</a> to come back to this page.</html>");
          client.stop();
          Serial.println("Testing Sony...");
          disconnect_from_ap();
          connect_to_camera_ap();
          while(!connected)
          {
            delay(50);
          }

          httpPost(startRecMode);
          delay(2000);
          httpPost(setShootMode);
          delay(400);            
          sony_shoot();

          Serial.println("Connecting back to main AP");
          disconnect_from_ap();
          connect_to_main_ap();
          print_camera_config();
          break;          
        }                                     
        else
        if (strstr(request_string, "Begin"))
        do_focus_bracketing();
        else
        if (strstr(request_string, "save_config"))
        {
            save_config();
        } 
        else
        if (strstr(request_string, "load_config"))
        {
            load_config();
        }
        else
        if (strstr(request_string, "toggle_light_1"))
        {
            if(light_1_state==0)
            digitalWrite(LIGHT_1_PIN, HIGH);
            else
            digitalWrite(LIGHT_1_PIN, LOW);

            light_1_state=!light_1_state;
        }
        else
        if (strstr(request_string, "toggle_light_2"))
        {
            if(light_2_state==0)
            digitalWrite(LIGHT_2_PIN, HIGH);
            else
            digitalWrite(LIGHT_2_PIN, LOW);

            light_2_state=!light_2_state;
        }                  
        else        
        if (strstr(request_string, "set_initial_delay="))
        {
            config.initial_delay=get_int_in_get(request_string,(char*)"set_initial_delay");
            
            Serial.print("Initial delay set to ");
            Serial.println(config.initial_delay);
        }        

        if (strstr(request_string, "set_delay_after_shutter="))
        {
            config.delay_after_shutter=get_int_in_get(request_string,(char*)"set_delay_after_shutter");
            
            Serial.print("After shutter delay set to ");
            Serial.println(config.delay_after_shutter);
        }   

        if (strstr(request_string, "set_delay_after_move="))
        {
            config.delay_after_move=get_int_in_get(request_string,(char*)"set_delay_after_move");
            
            Serial.print("After move delay set to ");
            Serial.println(config.delay_after_move);
        } 

        if (strstr(request_string, "set_total_photos="))
        {
            config.total_photos=get_int_in_get(request_string,(char*)"set_total_photos");
            
            Serial.print("Total photos set to ");
            Serial.println(config.total_photos);
        } 

        if (strstr(request_string, "set_steps_to_move="))
        {
            config.steps_to_move=get_int_in_get(request_string,(char*)"set_steps_to_move");
            
            Serial.print("Steps to move set to ");
            Serial.println(config.steps_to_move);
        } 

        if (strstr(request_string, "set_shutter_type="))
        {
            config.shutter_type=get_int_in_get(request_string,(char*)"set_shutter_type");
            
            Serial.print("Shutter type set to ");
            Serial.println(config.shutter_type);
        } 

        if (strstr(request_string, "set_camera_ip="))
        {
            get_string_in_get(request_string,(char*)"set_camera_ip", config.camera_ip, sizeof(config.camera_ip));
            
            Serial.print("Camera IP set to ");
            Serial.println(config.camera_ip);
        } 

        if (strstr(request_string, "set_ssid="))
        {
            get_string_in_get(request_string,(char*)"set_ssid", config.ssid, sizeof(config.ssid));
            
            Serial.print("SSID set to ");
            Serial.println(config.ssid);
        } 

        if (strstr(request_string, "set_pass="))
        {
            get_string_in_get(request_string,(char*)"set_pass", config.pass, sizeof(config.pass));
            
            Serial.print("Pass set to ");
            Serial.println(config.pass);
        } 

        if (strstr(request_string, "set_camera_ssid="))
        {
            get_string_in_get(request_string,(char*)"set_camera_ssid", config.camera_ssid, sizeof(config.camera_ssid));
            
            Serial.print("Camera SSID set to ");
            Serial.println(config.camera_ssid);
        }

        if (strstr(request_string, "set_camera_pass="))
        {
            get_string_in_get(request_string,(char*)"set_camera_pass", config.camera_pass, sizeof(config.camera_pass));
            
            Serial.print("Camera config.pass set to ");
            Serial.println(config.camera_pass);
        }

        if (strstr(request_string, "config_ssid"))
        {
          print_ssid_config_page();
          break;
        }

        if (strstr(request_string, "apply_net_stuff"))
        {
          Serial.println("Changing ssid/pass");
          apply_net_stuff();          
        }

        
        if (strstr(request_string, "apply_camera_stuff"))
        {
          apply_camera_stuff();
          break;
        }

        if (strstr(request_string, "camera_config"))
        {
          print_camera_config();
          break;          
        }                
        
        if (strstr(request_string, "flush_events"))
        {
          flush_events_buffer();   
        }     

          // send a standard http response header
          final_string[0]=0;
          sprintf(status_string,"<table><form action=\"/\" method=\"get\"><tr><td><label for=\"set_initial_delay\">Initial Delay:</label></td><td><input type=\"text\" name=\"set_initial_delay\" value=\"%i\"></td></tr>"
          "<tr><td><label for=\"set_delay_after_shutter\">After Shutter Delay:</label></td><td><input type=\"text\" name=\"set_delay_after_shutter\" value=\"%i\"></td></tr><tr>"
          "<td><label for=\"set_delay_after_move\">After Move Delay:</label></td><td><input type=\"text\" name=\"set_delay_after_move\" value=\"%i\"></td></tr>"
          "<tr><td><label for=\"set_total_photos\">Number of Photos:</label></td><td><input type=\"text\" name=\"set_total_photos\" value=\"%i\"></td></tr>"
          "<tr><td><label for=\"set_steps_to_move\">Steps between shots:</label></td><td><input type=\"text\" name=\"set_steps_to_move\" value=\"%i\"></td></tr>"
          "\n<tr><td><label for='set_shutter_type'>Shutter type:</label></td><td>"
          "\n<select name='set_shutter_type'>"
          "\n<option value='0'%s>None</option>"
          "\n<option value='1'%s>Phone BT</option>"
          "\n<option value='2'%s>Canon IR</option>"
          "\n<option value='3'%s>Nikon IR</option>"
          "\n<option value='4'%s>Panasonic (WiFi)</option>"
          "\n<option value='5'%s>Olympus (WiFi)</option>"
          "\n<option value='6'%s>Canon PTP IP</option>"
          "\n<option value='7'%s>Sony Camera Remote API</option>"
          "\n<option value='8'%s>Nikon PTP IP</option>"
          "\n</select></td></tr>"
          "\n<tr><td colspan='2'><input type=\"submit\" value=\"Apply\"></td><tr></form></table>",config.initial_delay,config.delay_after_shutter,config.delay_after_move,config.total_photos,config.steps_to_move,
          config.shutter_type==0?" selected":"",config.shutter_type==1?" selected":"",config.shutter_type==2?" selected":"",config.shutter_type==3?" selected":"",config.shutter_type==4?" selected":"",
          config.shutter_type==5?" selected":"",config.shutter_type==6?" selected":"",config.shutter_type==7?" selected":"",config.shutter_type==8?" selected":"");

          strcat(final_string,header_string);
          strcat(final_string,status_string);
          strcat(final_string,"\n<table><tr>"
          "\n<form action=\"/save_config\" method=\"get\"><td><input type=\"submit\" value=\"Save Config\"></form></td>"
          "\n<td><form action=\"/load_config\" method=\"get\"><input type=\"submit\" value=\"Load Config\"></form></td>"
          "\n<td><form action='/config_ssid' method='get'><input type='submit' value='Config SSID'></form></td>"
          "\n<td><form action='/camera_config' method='get'><input type='submit' value='Config Camera'></form></td>"
          "</tr></table>");          
          strcat(final_string,"\n<table><tr><td><form action=\"/move_backward=1000\" method=\"get\"><input type=\"submit\" value=\"<<<\"></form></td>"
          "<td><form action=\"/move_backward=100\" method=\"get\"><input type=\"submit\" value=\"<<\"></form></td><td><form action=\"/move_backward=5\" method=\"get\"><input type=\"submit\" value=\"<\"></form></td>"
          "<form action=\"/Begin\" method=\"get\"><td><input type=\"submit\" value=\"Begin\"></form></td><td><form action=\"/move_forward=5\" method=\"get\"><input type=\"submit\" value=\">\"></form></td><td><form action=\"/move_forward=100\" method=\"get\"><input type=\"submit\" value=\">>\"></form></td>"
          "<td><form action=\"/move_forward=1000\" method=\"get\"><input type=\"submit\" value=\">>>\"></form></td></tr><tr><td colspan='3'><form action=\"/toggle_light_1\" method=\"get\"><input type=\"submit\" value=\"Toggle light 1\"></form></td><td colspan='4'><form action=\"/toggle_light_2\" method=\"get\"><input type=\"submit\" value=\"Toggle light 2\"></form></td></tr></table>");
          /*
          sprintf(status_string,"<table><tr><td><form action='/' method=\"get\"><label for=\"set_camera_ip\">Camera IP:</label></td><td><input type=\"text\" name=\"set_camera_ip\" value=\"%s\"></td></tr>"
          "\n<tr><td><label for=\"set_camera_ssid\">Camera SSID:</label></td><td><input type=\"text\" name=\"set_camera_ssid\" value=\"%s\"></td></tr>"
          "\n<tr><td><label for=\"set_camera_pass\">Camera Pass:</label></td><td><input type=\"text\" name=\"set_camera_pass\" value=\"%s\"></td></tr>"
          "\n<tr><td><input type=\"submit\" value='Set'></form></td></tr></table></html>",config.camera_ip,config.camera_ssid,config.camera_pass);
          strcat(final_string,status_string);
          */
         strcat(final_string,"</html>");
          client.println(final_string);          
           break;
        }
        if (c == '\n') 
        {
          // you're starting a new line
          request_string[char_no]=0;//end the request string
          currentLineIsBlank = true;
          lines_no++;
        }
        else if (c != '\r') 
        {
          // you've gotten a character on the current line
          currentLineIsBlank = false;
        }
      }
    }
    // give the web browser time to receive the data
    delay(1);
   
    // close the connection:
    client.stop();
    //Serial.println("client disonnected");
    //digitalWrite(16, HIGH);
  }
  
}


void printWifiStatus() {
  // print the SSID of the network you're attached to:
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print your WiFi shield's IP address:
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  // print the received signal strength:
  long rssi = WiFi.RSSI();
  Serial.print("signal strength (RSSI):");
  Serial.print(rssi);
  Serial.println(" dBm");
}
