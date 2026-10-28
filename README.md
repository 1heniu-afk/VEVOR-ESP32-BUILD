# VEVOR ESP32 — publiczna budowa testowa (0.6.3)

To jest **projekt do weryfikacji kompilacji**, NIE potwierdzony sterownik ogrzewania.

## Bezpieczenstwo

- Nie udostepniaj hasel ani kluczy prywatnych. W publicznym projekcie nie ma oryginalnego hasla.
- ESP32 generuje wlasny 8-cyfrowy PIN przy pierwszym uruchomieniu i zachowuje go w NVS.
  Odczytaj ten PIN przez USB / Arduino Serial Monitor 115200 po pierwszym uruchomieniu.
  Ochrona PIN-em nie zastepuje parowania/uwierzytelnienia BLE; uzywaj tylko podczas testow pod nadzorem.
- OtaPublicKey.h zawiera tylko klucz **publiczny**; odpowiadajacy mu prywatny klucz OTA
  nie moze zostac opublikowany. Nie mozna automatycznie podpisac nowej wersji bez niego.
- Start/Stop musi byc potwierdzony przy fizycznym piecu. Nie zostawiaj grzania bez nadzoru.
- Pozostaw firmware diagnostyczny 0.2.3 dopoki kompilacja i testy na sprzecie nie powioda sie.

## Automatyczna kompilacja (GitHub Actions)

Push do glownej galezi wyzwala dwa zadania: `esp32-firmware` i `android-apk`.
Wyniki `.bin` oraz `app-debug.apk` pojawia sie jako **artifacts** TYLKO po udanej kompilacji.
Na GitHub publiczne artifacts i logi moga byc dostepne dla innych.

ESP32: `ESP32 Dev Module` 4MB, Arduino ESP32 **3.3.12**, Minimal SPIFFS 1.9MB OTA.
Biblioteki: OneWire 2.3.8, DallasTemperature 4.0.6, BLEOTA 1.0.6.
Android: Gradle 8.10.2, AGP 8.7.3, Java 17, compileSdk 35.

Firmware i APK nie zostaly jeszcze rzeczywiscie skompilowane i nie mozna gwarantowac dzialania.
