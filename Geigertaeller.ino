// Geigertæller v3 - Arduino Uno + J305 GM-rør-board + SSD1306 OLED (128x64, software-SPI)
//
// Nyt i v3:
//  - Stor dosisrate, statistisk usikkerhed (±%), samlet dosis og driftstid
//  - Graf over de seneste 16 minutter nederst på skærmen
//  - Automatisk gennemsnitsvindue: 60 s ved baggrund, kortere ved høj stråling,
//    så tælleren reagerer hurtigt, når du nærmer dig en kilde
//  - Alarm (blinkende skærm og evt. buzzer) over en valgfri grænse
//  - Pulsindikator i øverste højre hjørne

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------- Skærm (software-SPI) ----------
const uint8_t SCREEN_WIDTH  = 128;
const uint8_t SCREEN_HEIGHT = 64;
const uint8_t OLED_MOSI  = 9;
const uint8_t OLED_CLK   = 10;
const uint8_t OLED_DC    = 11;
const uint8_t OLED_CS    = 12;
const uint8_t OLED_RESET = 13;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT,
                         OLED_MOSI, OLED_CLK, OLED_DC, OLED_RESET, OLED_CS);

// ---------- Indstillinger ----------
const uint8_t GEIGER_PIN  = 2;        // Uno: kun pin 2 og 3 kan bruges til interrupt
const float   USV_PER_CPM = 0.00812;  // J305: ca. 123 CPM = 1 uSv/h
const float   ALARM_USV   = 0.5;      // Alarmgrænse i uSv/h
const uint8_t BUZZER_PIN  = 0;        // 0 = ingen buzzer. Fx 5, hvis du tilslutter en

// ---------- Måling ----------
const uint8_t MAX_WINDOW = 60;        // Længste gennemsnitsvindue i sekunder

volatile uint16_t pulseCount = 0;     // Tælles op i interruptet

uint8_t  bins[MAX_WINDOW];            // Pulser pr. sekund, de seneste 60 sekunder (ringbuffer)
                                      // (uint8_t sparer RAM; 255/s = 15.300 CPM er langt over normalt)
uint8_t  binIndex     = 0;            // Næste plads, der skrives til
uint8_t  binsFilled   = 0;            // Sekunder med data (op til 60)
uint8_t  validSeconds = 0;            // Sekunder siden sidste store ændring i niveau

uint32_t totalCounts = 0;
uint32_t runSeconds  = 0;
float    doseUsv     = 0;             // Samlet dosis siden start
uint32_t lastTick    = 0;

// ---------- Graf ----------
const uint8_t GRAPH_BARS        = 64;  // 64 søjler á 2 pixel = hele bredden
const uint8_t GRAPH_SECS_PR_BAR = 15;  // 64 x 15 s = 16 minutter
const uint8_t GRAPH_TOP         = 46;
const uint8_t GRAPH_HEIGHT      = SCREEN_HEIGHT - GRAPH_TOP;
uint16_t graph[GRAPH_BARS];            // CPM pr. søjle, nyeste sidst
uint8_t  graphBars  = 0;
uint32_t graphAccum = 0;
uint8_t  graphSecs  = 0;

// Ledig RAM mellem heap og stack (Uno har kun 2048 bytes i alt)
int freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

// Interrupt-rutine: så kort som muligt
void onPulse() {
  pulseCount++;
}

// Læs og nulstil tælleren, uden at et interrupt kan komme midt i
uint16_t takeCount() {
  noInterrupts();
  uint16_t c = pulseCount;
  pulseCount = 0;
  interrupts();
  return c;
}

// Summen af de seneste n sekunder
uint32_t sumLast(uint8_t n) {
  uint32_t s = 0;
  uint8_t i = binIndex;
  for (uint8_t k = 0; k < n; k++) {
    i = (i == 0) ? MAX_WINDOW - 1 : i - 1;
    s += bins[i];
  }
  return s;
}

// Vælg vindue ud fra niveauet de seneste 10 sekunder (samme idé som ZX Counter)
uint8_t targetWindow() {
  uint8_t n = min(binsFilled, (uint8_t)10);
  uint32_t cpm10 = sumLast(n) * 60UL / n;
  if (cpm10 < 60)   return 60;
  if (cpm10 < 300)  return 30;
  if (cpm10 < 1500) return 10;
  return 5;
}

void addGraphPoint(uint32_t cpm) {
  graphAccum += cpm;
  if (++graphSecs < GRAPH_SECS_PR_BAR) return;

  uint16_t value = graphAccum / GRAPH_SECS_PR_BAR;
  graphAccum = 0;
  graphSecs = 0;

  if (graphBars < GRAPH_BARS) {
    graph[graphBars++] = value;
  } else {
    memmove(graph, graph + 1, (GRAPH_BARS - 1) * sizeof(graph[0]));
    graph[GRAPH_BARS - 1] = value;
  }
}

void drawGraph() {
  uint16_t maxVal = 40;                      // Mindste skala (ca. 0,3 uSv/h), så baggrunden ikke fylder det hele
  for (uint8_t i = 0; i < graphBars; i++) {
    if (graph[i] > maxVal) maxVal = graph[i];
  }

  // Grundlinje
  display.drawFastHLine(0, SCREEN_HEIGHT - 1, SCREEN_WIDTH, SSD1306_WHITE);

  // Søjler, højrejusteret så den nyeste altid står yderst til højre
  uint8_t x0 = SCREEN_WIDTH - graphBars * 2;
  for (uint8_t i = 0; i < graphBars; i++) {
    uint8_t h = (uint32_t)graph[i] * (GRAPH_HEIGHT - 1) / maxVal;
    if (h > 0) {
      display.fillRect(x0 + i * 2, SCREEN_HEIGHT - 1 - h, 2, h, SSD1306_WHITE);
    }
  }
}

void printTime(uint32_t s) {
  uint32_t h = s / 3600;
  uint8_t  m = (s / 60) % 60;
  uint8_t  sec = s % 60;
  display.print(h);
  display.print(':');
  if (m < 10) display.print('0');
  display.print(m);
  display.print(':');
  if (sec < 10) display.print('0');
  display.print(sec);
}

void showReading(uint32_t cpm, float usv, uint32_t windowCounts, uint8_t window, bool pulse) {
  display.clearDisplay();
  display.setTextSize(1);

  // --- Øverste linje: driftstid, vindue og pulsindikator ---
  display.setCursor(0, 0);
  printTime(runSeconds);
  display.setCursor(92, 0);
  if (window < 10) display.print(' ');
  display.print(window);
  display.print('s');
  if (pulse) display.fillCircle(124, 3, 3, SSD1306_WHITE);

  // --- Dosisrate i stort ---
  display.setTextSize(3);
  display.setCursor(0, 11);
  if (usv < 10)        display.print(usv, 3);
  else if (usv < 100)  display.print(usv, 2);
  else                 display.print(usv, 1);

  display.setTextSize(1);
  display.setCursor(94, 11);
  display.write(0xE6);                       // µ i tegnsættet CP437
  display.print(F("Sv/h"));

  // Statistisk usikkerhed (1 sigma) = 1/sqrt(antal pulser i vinduet)
  display.setCursor(94, 22);
  if (windowCounts > 0) {
    uint16_t pct = 100.0 / sqrt((float)windowCounts) + 0.5;
    display.write(0xF1);                     // ± i tegnsættet CP437
    display.print(pct);
    display.print('%');
  } else {
    display.print(F("---"));
  }

  // --- CPM og samlet dosis ---
  display.setCursor(0, 37);
  display.print(cpm);
  display.print(F(" CPM"));
  display.setCursor(56, 37);
  display.print(F("Dose "));
  display.print(doseUsv, doseUsv < 10 ? 2 : (doseUsv < 100 ? 1 : 0));
  display.write(0xE6);
  display.print(F("Sv"));

  // --- Graf ---
  drawGraph();

  display.display();
}

// ---------- Load-skærm ----------

// Tegner strålingssymbolet (trefoil) med standardproportionerne:
// midtercirkel R, vingerne går fra 1,5R til 5R og er hver 60 grader brede.
// rot = vinklen for den første vinge (pi/2 = lige ned på skærmen)
void drawTrefoil(int16_t cx, int16_t cy, uint8_t rOut, float rot) {
  const uint8_t STEPS = 8;                   // Trekanter pr. vinge (glatter buen)
  const float   BLADE = PI / 3;              // 60 grader

  for (uint8_t b = 0; b < 3; b++) {
    float a = rot + b * (2 * PI / 3) - BLADE / 2;
    int16_t x1 = cx + rOut * cos(a) + 0.5;
    int16_t y1 = cy + rOut * sin(a) + 0.5;
    for (uint8_t s = 0; s < STEPS; s++) {
      a += BLADE / STEPS;
      int16_t x2 = cx + rOut * cos(a) + 0.5;
      int16_t y2 = cy + rOut * sin(a) + 0.5;
      display.fillTriangle(cx, cy, x1, y1, x2, y2, SSD1306_WHITE);
      x1 = x2;
      y1 = y2;
    }
  }
  display.fillCircle(cx, cy, rOut * 3 / 10, SSD1306_BLACK);  // Hul mellem vinger og midte
  display.fillCircle(cx, cy, rOut / 5, SSD1306_WHITE);       // Midtercirkel
}

void splashScreen() {
  const uint16_t SPLASH_MS = 4000;
  uint32_t start = millis();
  uint32_t elapsed;

  do {
    elapsed = millis() - start;
    if (elapsed > SPLASH_MS) elapsed = SPLASH_MS;
    float p = elapsed / (float)SPLASH_MS;    // 0.0 -> 1.0

    display.clearDisplay();

    // Symbolet drejer en tredjedel omgang og ender i standardstilling
    drawTrefoil(26, 32, 24, HALF_PI + p * (2 * PI / 3));

    display.setTextSize(2);
    display.setCursor(56, 4);
    display.print(F("GEIGER"));
    display.setTextSize(1);
    display.setCursor(56, 22);
    display.print(F("Counter J305"));

    // Fremskridtslinje
    display.drawRect(56, 36, 72, 8, SSD1306_WHITE);
    display.fillRect(58, 38, (uint8_t)(68 * p), 4, SSD1306_WHITE);

    // Status: viser pulser under opstarten, så du kan se, at røret virker
    display.setCursor(56, 52);
    if (p < 0.3) {
      display.print(F("Starting..."));
    } else if (p < 1.0) {
      noInterrupts();
      uint16_t n = pulseCount;
      interrupts();
      display.print(F("Pulses: "));
      display.print(n);
    } else {
      display.print(F("Ready!"));
    }

    display.display();
  } while (elapsed < SPLASH_MS);

  delay(700);                                // Lad "Ready!" stå et øjeblik
}

void setup() {
  Serial.begin(9600);

  // display.begin() returnerer false, hvis der ikke er RAM nok til skærmbufferen
  if (!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println(F("SSD1306 kunne ikke startes"));
    while (true) {}
  }

  Serial.print(F("Ledig RAM: "));
  Serial.println(freeRam());

  display.setTextColor(SSD1306_WHITE);
  display.cp437(true);                       // Korrekt tegnsæt, så µ og ± kan vises
  if (BUZZER_PIN) pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GEIGER_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(GEIGER_PIN), onPulse, FALLING);

  splashScreen();                            // Tæller allerede pulser = test af røret
  takeCount();                               // Nulstil, så målingen starter forfra

  Serial.println(F("cpm,usv,window,total"));
  lastTick = millis();
}

void loop() {
  if (millis() - lastTick < 1000) return;
  lastTick += 1000;                 // += i stedet for = millis(), så tiden ikke driver
  runSeconds++;

  uint16_t c = takeCount();
  totalCounts += c;

  // Læg det nye sekund i ringbufferen
  bins[binIndex] = c > 255 ? 255 : c;
  binIndex = (binIndex + 1) % MAX_WINDOW;
  if (binsFilled < MAX_WINDOW) binsFilled++;
  if (validSeconds < MAX_WINDOW) validSeconds++;

  // Ved et spring i niveau skrumper vinduet straks, og vokser derefter
  // et sekund ad gangen, så gamle data ikke trækker tallet med sig
  uint8_t target = targetWindow();
  if (validSeconds > target) validSeconds = target;
  uint8_t window = validSeconds;

  uint32_t windowCounts = sumLast(window);
  uint32_t cpm = windowCounts * 60UL / window;
  float usv = cpm * USV_PER_CPM;

  doseUsv += usv / 3600.0;
  addGraphPoint(cpm);

  // Alarm: blinkende (inverteret) skærm og evt. bip
  bool alarm = usv >= ALARM_USV;
  display.invertDisplay(alarm && (runSeconds & 1));
  if (alarm && BUZZER_PIN) tone(BUZZER_PIN, 2500, 150);

  showReading(cpm, usv, windowCounts, window, c > 0);

  // CSV til Serial Monitor / Serial Plotter
  Serial.print(cpm);
  Serial.print(',');
  Serial.print(usv, 3);
  Serial.print(',');
  Serial.print(window);
  Serial.print(',');
  Serial.println(totalCounts);
}
