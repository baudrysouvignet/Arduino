#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFiS3.h>
#include <PubSubClient.h>
#include <U8g2lib.h>
#include "Arduino_LED_Matrix.h"

const char ssid[] = "iPhone";
const char pass[] = "Baudsouvv";
const char *mqtt_broker = "broker.emqx.io";
const int   mqtt_port   = 1883;

const char *TOPIC_BPM    = "dataCastres/bpm";
const char *TOPIC_BOUTON = "dataCastres/bouton";  
const char *TOPIC_LED    = "dataCastres/led";
const char *TOPIC_RFID   = "dataCastres/rfid";     

const int BOUTON   = 2;    
const int LED      = 9;   
const int RFID_NSS = 10;
const int RFID_RST = 7;
const int JOY_X    = A0;

MFRC522 rfid(RFID_NSS, RFID_RST);
U8G2_SSD1306_128X64_ALT0_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
ArduinoLEDMatrix matrix;
const uint32_t PLEIN[3] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
const uint32_t VIDE[3]  = {0, 0, 0};

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
String client_id;

enum Etat { ATTENTE, START, MESURE, STOP };
Etat etat = ATTENTE;
unsigned long etatDepuis = 0;
const unsigned long DUREE_MESSAGE = 3000;

const int NB_PAGES = 3;
int page = 0;                     
const char *TITRES[NB_PAGES] = {"BPM", "KM parcourus", "KM/H moyen"};
float kmParcourus = 2.4;                   // valeur fixe pour l'instant
float vitesseMoyenne = 8.5;
bool joyAuCentre = true;

bool actif = false;
bool ecranOn = true;
bool dernierEtatBouton = LOW;
int bpm = 0;
unsigned long derniereLecture = 0;
unsigned long dernierBattement = 0;
unsigned long lastReconnect = 0;
unsigned long dernierScan = 0;
unsigned long dernierCheckRFID = 0;

void texteCentre(const char *s, int y) {
  oled.drawStr((128 - oled.getStrWidth(s)) / 2, y, s);
}

void afficherMessage(const char *ligne1, const char *ligne2) {
  if (!ecranOn) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_ncenB10_tr);
  texteCentre(ligne1, 28);
  oled.setFont(u8g2_font_6x10_tr);
  texteCentre(ligne2, 50);
  oled.sendBuffer();
}

void dessinerPoints() {
  for (int i = 0; i < NB_PAGES; i++) {
    int x = 64 + (i - 1) * 10;
    if (i == page) oled.drawDisc(x, 60, 2);
    else           oled.drawCircle(x, 60, 2);
  }
}

void afficher() {
  if (!ecranOn) return;
  oled.clearBuffer();
  switch (etat) {
    case ATTENTE:
      oled.setFont(u8g2_font_ncenB14_tr);
      texteCentre("En attente", 28);
      oled.setFont(u8g2_font_6x10_tr);
      texteCentre("Scannez une carte", 52);
      break;

    case START:
      oled.setFont(u8g2_font_logisoso24_tr);
      texteCentre("START", 44);
      break;

    case MESURE: {
      oled.setFont(u8g2_font_4x6_tr);
      texteCentre(TITRES[page], 10);

      String valeur;
      if (page == 0)      valeur = (bpm > 0) ? String(bpm) : "--";
      else if (page == 1) valeur = String(kmParcourus, 1);
      else                valeur = String(vitesseMoyenne, 1);

      oled.setFont(u8g2_font_logisoso24_tr);
      texteCentre(valeur.c_str(), 58);
      dessinerPoints();
      break;
    }

    case STOP:
      oled.setFont(u8g2_font_logisoso24_tr);
      texteCentre("STOP", 44);
      break;
  }
  oled.sendBuffer();
}

void changerEtat(Etat e) {
  etat = e;
  etatDepuis = millis();
  afficher();
}

void lireJoystick() {
  int x = analogRead(JOY_X);

  if (joyAuCentre && (x < 300 || x > 700)) {
    joyAuCentre = false;
    if (x > 700) page = (page + 1) % NB_PAGES;
    else         page = (page + NB_PAGES - 1) % NB_PAGES;
    Serial.print("Page : "); Serial.println(TITRES[page]);
    if (etat == MESURE) afficher();
  }
  else if (x > 400 && x < 620) {
    joyAuCentre = true;
  }
}

bool lecteurOK() {
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  return v != 0x00 && v != 0xFF;
}

void initRFID() {
  for (int essai = 1; essai <= 5; essai++) {
    rfid.PCD_Init();
    delay(50);
    byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.print("Lecteur RFID, essai "); Serial.print(essai);
    Serial.print(" : version 0x"); Serial.println(v, HEX);
    if (v != 0x00 && v != 0xFF) {
      rfid.PCD_SetAntennaGain(rfid.RxGain_max);
      Serial.println("Lecteur RFID OK");
      return;
    }
    delay(500);
  }
  Serial.println("Lecteur RFID NON DETECTE (verifier fils / soudures)");
}

String lireUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void connectWiFi() {
  Serial.print("Connexion WiFi");
  afficherMessage("Connexion WiFi", "en cours...");
  while (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid, pass);
    delay(3000);
    Serial.print(".");
  }
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0)) delay(200);

  Serial.print("\nWiFi OK, IP : ");
  Serial.println(WiFi.localIP());
  String ip = WiFi.localIP().toString();
  afficherMessage("WiFi OK", ip.c_str());
  delay(1000);
}

void publishBouton() {
  if (mqtt.connected()) mqtt.publish(TOPIC_BOUTON, actif ? "on" : "off");
}

bool connectMQTT() {
  if (mqtt.connect(client_id.c_str())) {
    Serial.println("MQTT OK");
    mqtt.subscribe(TOPIC_LED);
    publishBouton();
    return true;
  }
  Serial.print("Echec MQTT, rc=");
  Serial.println(mqtt.state());
  return false;
}

void callback(char *topic, byte *payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();
  Serial.print("<- "); Serial.print(topic); Serial.print(" : "); Serial.println(msg);
  if (String(topic) == TOPIC_LED) matrix.loadFrame(msg == "on" ? PLEIN : VIDE);
}

void basculerPartie() {
  actif = !actif;
  if (actif) {
    page = 0;
    Serial.println("PARTIE DEMARREE");
    changerEtat(START);
  } else {
    bpm = 0;
    Serial.println("PARTIE ARRETEE");
    changerEtat(STOP);
  }
  publishBouton();
}

void lireRFID() {
  if (millis() - dernierCheckRFID >= 5000) {
    dernierCheckRFID = millis();
    if (!lecteurOK()) {
      Serial.println("Lecteur RFID perdu -> reinitialisation");
      initRFID();
    }
  }

  if (millis() - dernierScan < 1500) return;
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  dernierScan = millis();

  String uid = lireUID();
  Serial.print("Carte UID : ");
  Serial.println(uid);
  if (mqtt.connected()) {
    mqtt.publish(TOPIC_RFID, uid.c_str());   
    Serial.println("-> rfid publie");
  }

  basculerPartie();
  rfid.PICC_HaltA();
}

void setup() {
  pinMode(BOUTON, INPUT);
  pinMode(LED, OUTPUT);
  Serial.begin(115200);
  delay(3000);

  Wire.begin();
  oled.begin();
  afficherMessage("Demarrage", "patientez...");

  SPI.begin();
  matrix.begin();
  matrix.loadFrame(VIDE);

  randomSeed(analogRead(A3));
  client_id = "dataCastres-A1-" + String(random(0xFFFF), HEX);

  connectWiFi();

  afficherMessage("Connexion MQTT", "en cours...");
  mqtt.setServer(mqtt_broker, mqtt_port);
  mqtt.setCallback(callback);
  connectMQTT();

  initRFID();
  changerEtat(ATTENTE);
  Serial.println("Scanne une carte pour demarrer");
}

void loop() {
  unsigned long maintenant = millis();

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    afficher();
  }

  if (!mqtt.connected()) {
    if (maintenant - lastReconnect >= 5000) { lastReconnect = maintenant; connectMQTT(); }
  } else {
    mqtt.loop();
  }

  lireRFID();
  lireJoystick();

  bool etatBouton = digitalRead(BOUTON);
  if (etatBouton == HIGH && dernierEtatBouton == LOW) {
    ecranOn = !ecranOn;
    oled.setPowerSave(ecranOn ? 0 : 1);
    Serial.println(ecranOn ? "Ecran ON" : "Ecran OFF");
    if (ecranOn) afficher();
    delay(50);
  }
  dernierEtatBouton = etatBouton;

  if (etat == START && maintenant - etatDepuis >= DUREE_MESSAGE) changerEtat(MESURE);
  if (etat == STOP  && maintenant - etatDepuis >= DUREE_MESSAGE) changerEtat(ATTENTE);

  if (!actif) { digitalWrite(LED, LOW); return; }

  if (maintenant - derniereLecture >= 1000) {
    derniereLecture = maintenant;
    if (Wire.requestFrom(0x50, 1) && Wire.available()) {
      int nouveau = Wire.read();
      Serial.print("BPM : "); Serial.println(nouveau);
      if (mqtt.connected()) mqtt.publish(TOPIC_BPM, String(nouveau).c_str());
      if (nouveau != bpm) {
        bpm = nouveau;
        if (etat == MESURE && page == 0) afficher();
      }
    }
  }

  if (bpm > 0) {
    unsigned long periode = 60000UL / bpm;
    if (maintenant - dernierBattement >= periode) dernierBattement = maintenant;
    digitalWrite(LED, (maintenant - dernierBattement) < 100 ? HIGH : LOW);
  } else {
    digitalWrite(LED, LOW);
  }
}
