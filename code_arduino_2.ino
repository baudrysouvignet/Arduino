#include <WiFiS3.h>
#include <PubSubClient.h>

//const char ssid[] = "iPhone de kh";
//const char pass[] = "1234@@@@";
const char ssid[] = "iPhone";
const char pass[] = "Baudsouvv";

// ---- MQTT ----
const char *mqtt_broker = "broker.emqx.io";
const int   mqtt_port   = 1883;
const char *TOPIC_ALARME_CMD = "dataCastres/alarme_cmd";

// ---- Matériel ----
const int BUZZER = 3;

// ---- Sirène ----
const int FREQ_MIN = 600;               // Hz
const int FREQ_MAX = 1500;              // Hz
const unsigned long DELAI_ETAPE = 10;   // ms entre deux changements de fréquence

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
String client_id;

bool alarmeActive = false;              // piloté par Node-RED via MQTT
bool etaitActive  = false;              // détecte le moment où l'alarme s'arrête
int frequence = FREQ_MIN;
int pas = 20;
unsigned long derniereEtape = 0;
unsigned long lastReconnect = 0;

void connectWiFi() {
  noTone(BUZZER);                       // silence pendant la reconnexion
  Serial.print("Connexion WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid, pass);
    delay(3000);
    Serial.print(".");
  }
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0)) delay(200);
  Serial.print("\nWiFi OK, IP : ");
  Serial.println(WiFi.localIP());
}

bool connectMQTT() {
  if (mqtt.connect(client_id.c_str())) {
    Serial.println("MQTT OK");
    mqtt.subscribe(TOPIC_ALARME_CMD);
    return true;
  }
  Serial.print("Echec MQTT, rc=");
  Serial.println(mqtt.state());
  return false;
}

void callback(char *topic, byte *payload, unsigned int length) {
  String msg;
  msg.reserve(length);                       // évite les réallocations mémoire
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  msg.trim();

  // --- Message "bip" ---
  if (msg == "bip") {
    if (!alarmeActive) {                     // si la sirène tourne déjà, inutile
      tone(BUZZER, 1000, 200);     // avec durée : non bloquant
    }
    Serial.println("bip");
    return;
  }

  // --- Message "1" (alarme ON) ou autre (alarme OFF) ---
  bool nouvelEtat = (msg == "1");
  if (nouvelEtat != alarmeActive) {          // n'affiche que les changements
    alarmeActive = nouvelEtat;
    Serial.println(alarmeActive ? "ALARME ON" : "alarme off");
  }
}                                            // <-- l'accolade qui manquait

// Sirène qui monte puis redescend, sans bloquer la boucle
void sirene() {
  if (millis() - derniereEtape >= DELAI_ETAPE) {
    derniereEtape = millis();
    frequence += pas;

    if (frequence >= FREQ_MAX) {
      frequence = FREQ_MAX;
      pas = -abs(pas);
    } else if (frequence <= FREQ_MIN) {
      frequence = FREQ_MIN;
      pas = abs(pas);
    }
    tone(BUZZER, frequence);
  }
}

void setup() {
  pinMode(BUZZER, OUTPUT);
  Serial.begin(115200);
  delay(3000);

  randomSeed(analogRead(A0));
  client_id = "dataCastres-A2-" + String(random(0xFFFF), HEX);

  connectWiFi();
  mqtt.setServer(mqtt_broker, mqtt_port);
  mqtt.setCallback(callback);
  connectMQTT();
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (!mqtt.connected()) {
    if (now - lastReconnect >= 5000) {
      lastReconnect = now;
      connectMQTT();
    }
  } else {
    mqtt.loop();
  }

  if (alarmeActive) {
    sirene();
  } else if (etaitActive) {             // l'alarme vient de s'arrêter
    noTone(BUZZER);
    frequence = FREQ_MIN;
    pas = abs(pas);
  }
  etaitActive = alarmeActive;
}
