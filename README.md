# Matter over Thread – jednobarevný LED pásek na ESP32-C6

Firmware vytváří Matter zařízení typu **Dimmable Light** (on/off + stmívání),
které komunikuje po **Thread** síti. Ovládat ho půjde z Apple Home, Google
Home, Home Assistant (přes Thread Border Router) apod.

## Co je potřeba

- **ESP32-C6** dev board (má 802.15.4 rádio pro Thread)
- Jednobarevný 12V/24V LED pásek + zdroj
- N-kanálový MOSFET (např. IRLZ44N, IRLB8721) k spínání pásku
- Thread Border Router v síti (např. Apple TV/HomePod mini, Google Nest Hub,
  nebo ESP32 s firmwarem OpenThread Border Router) – bez něj Matter-over-
  -Thread zařízení nepůjde spárovat s cloudovými ekosystémy
- Nainstalované **ESP-IDF** (v5.1+) a **esp-matter** SDK

## Zapojení

```
ESP32-C6 GPIO8 --[ 1kΩ ]--> Gate MOSFETu
                             Drain MOSFETu -> (-) LED pásku
                             Source MOSFETu -> GND
(+) LED pásku -> přímo na + napájecího zdroje pásku
GND zdroje pásku propojit se GND ESP32-C6 (společná zem!)
```

Pokud používáš jiný GPIO, uprav `LED_STRIP_GPIO` v `main/app_priv.h`.

## Instalace nástrojů (jen poprvé)

```bash
# ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && git checkout v5.5.4 && git submodule update --init --recursive
./install.sh
source ./export.sh
cd ..

# ESP-Matter SDK
git clone --depth 1 https://github.com/espressif/esp-matter.git
cd esp-matter
git submodule update --init --depth 1
cd connectedhomeip/connectedhomeip
./scripts/checkout_submodules.py --platform esp32 linux --shallow   # macOS: darwin
cd ../..
./install.sh
cd ..
export ESP_MATTER_PATH=$(pwd)/esp-matter
```

Tento krok (`source ./export.sh` u esp-idf a nastavení `ESP_MATTER_PATH`)
je potřeba zopakovat v každém novém terminálu.

## Umístění a sestavení projektu

Tento projekt zkopíruj do libovolné složky vedle `esp-matter/examples/`,
protože `CMakeLists.txt` očekává esp-matter komponenty 3 úrovně výš
(stejně jako oficiální příklady). Nejjednodušší je umístit ho přímo do:

```bash
cp -r matter_led_c6 $ESP_MATTER_PATH/examples/matter_led_c6
cd $ESP_MATTER_PATH/examples/matter_led_c6
```

Sestavení s Thread konfigurací pro ESP32-C6:

```bash
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6_thread" set-target esp32c6
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

(Port uprav podle svého systému – na Windows/WSL např. `COM5`.)

## Párování (commissioning)

Po nahrání firmware zařízení vypíše do sériové konzole **QR kód** a
**manuální párovací kód** (formát `MT:...`). Naskenuj ho v aplikaci:

- **Apple Home** – Přidat příslušenství → Skenovat QR kód
- **Google Home** – Přidat zařízení → Nastavit zařízení Matter
- **Home Assistant** – integrace *Matter (BETA)*, tlačítko „Přidat zařízení“

Zařízení se BLE připojí ke commissioneru a ten mu následně předá přihlašovací
údaje k Thread síti (proto je potřeba mít v dosahu Thread Border Router).

## Co firmware umí

| Matter cluster        | Funkce                                   |
|------------------------|-------------------------------------------|
| On/Off                | zapnutí/vypnutí LED pásku                 |
| Level Control          | plynulé stmívání 0–254 → PWM 0–100 %      |
| Identify               | reakce na „Identify“ (lze doplnit blikání)|

Řízení jasu je realizováno přes LEDC PWM periferii (`main/app_driver.cpp`),
5 kHz nosná frekvence (mimo slyšitelné pásmo, bez blikání pásku).

## Přizpůsobení

- **Jiný GPIO / PWM parametry** → `main/app_priv.h`
- **RGB nebo RGBW pásek** místo jedné barvy → bylo by potřeba použít
  `extended_color_light` endpoint a 3–4 kanály LEDC místo jednoho; rád ti
  to připravím, pokud budeš chtít.
- **Fyzické tlačítko** pro factory reset / identify → připravena kostra
  `app_driver_button_init()` v `app_priv.h`, stačí doplnit GPIO a ISR podle
  potřeby (např. pomocí komponenty `iot_button` z ESP-IDF component registry).

## Sestavení v cloudu přes GitHub Actions (bez instalace čehokoliv lokálně)

Pokud si nechceš instalovat ESP-IDF/ESP-Matter na svém počítači, můžeš nechat
firmware zkompilovat zdarma na GitHubu:

1. Založ si účet na [github.com](https://github.com) (pokud ho nemáš).
2. Vytvoř nový **prázdný veřejný repozitář** (např. `matter-led-c6`).
3. Nahraj do něj celý obsah této složky (`matter_led_c6/`) i s podsložkou
   `.github/workflows/build.yml` – nejjednodušší je na stránce repozitáře
   kliknout na **"Add file" → "Upload files"** a přetáhnout tam všechny
   soubory a složky (včetně skryté `.github`).
4. Po nahrání se automaticky (nebo přes záložku **Actions → Run workflow**,
   pokud se nespustí sama) spustí sestavení. Trvá cca 30–60 minut (klonují
   se velké repozitáře ESP-IDF a ConnectedHomeIP).
5. Až doběhne (zelená fajfka), otevři daný běh v záložce **Actions**,
   sjeď dolů na sekci **Artifacts** a stáhni **`matter_led_c6_firmware.zip`**.
6. Rozbal ho – uvnitř najdeš:
   - `bootloader.bin`
   - `partition-table.bin`
   - `matter_led_c6.bin`
   - `flash_args` (obsahuje přesné offsety, na které soubory patří)

### Nahrání přes ESP Web Flash

1. Otevři [ESP Web Flash Tool](https://espressif.github.io/esptool-js/) (nebo
   `https://web.esphome.io`, oba fungují na WebSerial).
2. Připoj ESP32-C6 přes USB, klikni **Connect**, vyber sériový port.
3. Přidej tři soubory na tyto offsety (podle `flash_args`):
   - `0x0` → `bootloader.bin`
   - `0x8000` → `partition-table.bin`
   - `0x20000` → `matter_led_c6.bin`
4. Klikni **Program/Flash** a počkej na dokončení.
5. Po restartu zařízení sleduj sériovou konzoli (Monitor) – vypíše se QR kód
   a párovací kód pro Matter commissioning.

## Poznámka k verzím API


Espressif poměrně často upravuje API esp-matter SDK (názvy konfiguračních
voleb, struktury `config_t` u endpointů). Kód vychází z aktuálně
dokumentované struktury `dimmable_light` a `OnOff`/`LevelControl` clusterů;
pokud narazíš při buildu na chybu typu neznámé pole ve struktuře, nejspíš se
jedná o drobný rozdíl oproti verzi SDK, kterou máš nainstalovanou – v tom
případě mi pošli chybovou hlášku z `idf.py build` a opravím to.
