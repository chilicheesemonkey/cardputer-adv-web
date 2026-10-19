// Cardputer ADV text browser
//  - Direct mode: device downloads the page and renders it itself (streaming HTML parser)
//  - Proxy mode : a Wasmer-hosted proxy (see /proxy) returns pre-simplified blocks
// UI is keyboard-only: WiFi setup, URL entry, links, history, bookmarks, settings.

#include <Arduino.h>
#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <vector>

#define GFX M5Cardputer.Display

// ---------------------------------------------------------------- constants
static const int COLS = 40;            // 240px / 6px font
static const int VIS = 14;             // visible text rows
static const int TOP_H = 11, LINE_H = 8, Y0 = 12;
static const int BOT_Y = Y0 + VIS * LINE_H;  // 124
static const int MAX_LINES = 1200, MAX_LINKS = 1500;
static const size_t MAX_BYTES = 300000;
static const char* UA = "Mozilla/5.0 (X11; Linux) CardputerBrowser/1.0";
static const char* SEARCH = "https://lite.duckduckgo.com/lite/?q=";

static const uint16_t C_WHITE = 0xFFFF, C_BLACK = 0x0000, C_CYAN = 0x07FF, C_YELLOW = 0xFFE0,
                      C_RED = 0xF800, C_GREEN = 0x07E0, C_ORANGE = 0xFD20, C_GREY = 0x8410,
                      C_LGREY = 0xC618, C_NAVY = 0x0010;

struct Line { char kind; String t; };
struct Link { String url; int line; };
struct Key { char c = 0; bool enter = false, del = false, tab = false, fn = false, esc = false; };

static std::vector<Line> page;
static std::vector<Link> links;
static String pageTitle, curUrl;
static int scrollTop = 0, selLink = -1;
static bool gFull = false;
static std::vector<String> history;
static std::vector<int> histScroll;
static int histPos = -1;

static Preferences prefs;
static String cfgSsid, cfgPass, cfgProxy, cfgKey, cfgHome;
static bool cfgProxyMode = false;

// ---------------------------------------------------------------- small UI helpers
static void bar(int y, int h, uint16_t bg, const String& s, uint16_t fg = C_WHITE) {
  GFX.fillRect(0, y, 240, h, bg);
  GFX.setTextColor(fg, bg);
  GFX.setCursor(2, y + 1);
  String t = s.length() > (unsigned)(COLS - 1) ? s.substring(0, COLS - 1) : s;
  GFX.print(t.c_str());
}
static void status(const String& s) { bar(BOT_Y, 135 - BOT_Y, C_NAVY, s); }

// Returns true when a key event (or auto-repeat) is available.
// Does its own edge detection (isChange() is unreliable on some keyboards), with debounce.
static bool getKey(Key& k) {
  static String lastSig = "";
  static uint32_t tPress = 0, tEmit = 0, tRelease = 0;
  static const uint32_t DEBOUNCE = 150, REPEAT_DELAY = 550, REPEAT_RATE = 90;
  M5Cardputer.update();
  auto& kb = M5Cardputer.Keyboard;
  uint32_t now = millis();
  if (!kb.isPressed()) {
    if (lastSig.length()) { tRelease = now; }
    lastSig = "";
    return false;
  }
  auto st = kb.keysState();
  k = Key();
  k.enter = st.enter; k.del = st.del; k.tab = st.tab; k.fn = st.fn;
  if (!st.word.empty()) k.c = st.word.back();
  if (st.fn && k.c == '`') { k.esc = true; k.c = 0; }
  bool real = k.c || k.enter || k.del || k.tab || k.esc;
  if (!real) return false;

  String sig; sig += k.c; sig += k.enter; sig += k.del; sig += k.tab; sig += k.esc;
  if (sig != lastSig) {                       // new press
    if (now - tEmit < DEBOUNCE) { lastSig = sig; return false; }   // contact bounce / double scan
    lastSig = sig; tPress = tEmit = now;
    return true;
  }
  // same key still held: auto-repeat (not for Enter/Tab)
  if (k.enter || k.tab) return false;
  if (now - tPress < REPEAT_DELAY || now - tEmit < REPEAT_RATE) return false;
  tEmit = now;
  return true;
}

// Pick-from-list menu. Returns index, -1 on cancel, 1000+idx when 'x' pressed (if del).
static int menu(const String& title, const std::vector<String>& items, int sel = 0, bool del = false) {
  int top = 0; bool redraw = true; Key k;
  while (true) {
    if (sel < top) top = sel;
    if (sel >= top + VIS) top = sel - VIS + 1;
    if (redraw) {
      bar(0, TOP_H, C_NAVY, title);
      for (int r = 0; r < VIS; r++) {
        int i = top + r, y = Y0 + r * LINE_H; bool s = (i == sel);
        GFX.fillRect(0, y, 240, LINE_H, s ? C_CYAN : C_BLACK);
        if (i < (int)items.size()) {
          GFX.setTextColor(s ? C_BLACK : C_WHITE, s ? C_CYAN : C_BLACK);
          GFX.setCursor(0, y);
          String t = items[i].length() > (unsigned)COLS ? items[i].substring(0, COLS) : items[i];
          GFX.print(t.c_str());
        }
      }
      status(del ? "; . move  Enter ok  x del  Del back" : "; . move  Enter select  Del back");
      redraw = false;
    }
    if (!getKey(k)) { delay(8); continue; }
    redraw = true;
    if (k.enter) return items.empty() ? -1 : sel;
    if (k.del || k.esc) return -1;
    if (k.c == ';') { if (sel > 0) sel--; }
    else if (k.c == '.') { if (sel + 1 < (int)items.size()) sel++; }
    else if (del && k.c == 'x' && !items.empty()) return 1000 + sel;
    else redraw = false;
  }
}

// Single/multi-line text entry. Enter = ok, Fn+` = cancel, Tab = clear, Del = backspace.
static bool editLine(const String& prompt, String& val) {
  String v = val; Key k; bool redraw = true;
  const int W = COLS - 1;
  while (true) {
    if (redraw) {
      bar(0, TOP_H, C_NAVY, prompt);
      GFX.fillRect(0, Y0, 240, VIS * LINE_H, C_BLACK);
      String shown = v + "_"; int n = shown.length();
      int rows = (n + W - 1) / W; int start = rows > VIS ? rows - VIS : 0;
      GFX.setTextColor(C_WHITE, C_BLACK);
      for (int r = start; r < rows; r++) {
        int e = (r + 1) * W; if (e > n) e = n;
        GFX.setCursor(0, Y0 + (r - start) * LINE_H);
        GFX.print(shown.substring(r * W, e).c_str());
      }
      status("Enter ok Del erase Tab clr Fn+` esc");
      redraw = false;
    }
    if (!getKey(k)) { delay(8); continue; }
    if (k.enter) { val = v; return true; }
    if (k.esc) return false;
    if (k.del) { if (v.length()) v.remove(v.length() - 1); }
    else if (k.tab) v = "";
    else if (k.c >= 32 && k.c < 127) v += k.c;
    redraw = true;
  }
}

// ---------------------------------------------------------------- URL helpers
static String urlEncode(const String& s) {
  String o; char b[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((uint8_t)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { snprintf(b, sizeof(b), "%%%02X", (uint8_t)c); o += b; }
  }
  return o;
}

static String resolveUrl(const String& base, String ref) {
  ref.trim();
  int h = ref.indexOf('#'); if (h >= 0) ref = ref.substring(0, h);
  if (!ref.length()) return "";
  String lr = ref; lr.toLowerCase();
  if (lr.startsWith("http://") || lr.startsWith("https://")) return ref;
  if (lr.startsWith("javascript:") || lr.startsWith("mailto:") || lr.startsWith("tel:") || lr.startsWith("data:")) return "";
  int se = base.indexOf("://"); if (se < 0) return "";
  String scheme = base.substring(0, se);
  if (ref.startsWith("//")) return scheme + ":" + ref;
  int pe = base.indexOf('/', se + 3);
  String origin = pe < 0 ? base : base.substring(0, pe);
  String bpath = pe < 0 ? String("/") : base.substring(pe);
  int q = bpath.indexOf('?'); if (q >= 0) bpath = bpath.substring(0, q);
  if (ref.startsWith("?")) return origin + bpath + ref;
  String path;
  if (ref.startsWith("/")) path = ref;
  else path = bpath.substring(0, bpath.lastIndexOf('/') + 1) + ref;
  String query; int qi = path.indexOf('?');
  if (qi >= 0) { query = path.substring(qi); path = path.substring(0, qi); }
  std::vector<String> seg; int i = 0;
  while (i <= (int)path.length()) {
    int j = path.indexOf('/', i); if (j < 0) j = path.length();
    String s = path.substring(i, j);
    if (s == "..") { if (!seg.empty()) seg.pop_back(); }
    else if (s != "." && s.length()) seg.push_back(s);
    i = j + 1;
  }
  String out; for (auto& s : seg) { out += "/"; out += s; }
  if (!out.length()) out = "/";
  else if (path.endsWith("/")) out += "/";
  return origin + out + query;
}

static String normalizeInput(String s) {
  s.trim();
  if (!s.length()) return s;
  if (s.startsWith("about:") || s.indexOf("://") > 0) return s;
  if (s.indexOf(' ') < 0 && (s.indexOf('.') > 0 || s.startsWith("localhost")))
    return String(isdigit((uint8_t)s[0]) ? "http://" : "https://") + s;
  return String(SEARCH) + urlEncode(s);
}

// ---------------------------------------------------------------- text normalisation
static const char* LAT = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiiidnooooo/ouuuuyty";

static void cpAscii(uint32_t cp, String& o) {
  if (cp < 32) { o += ' '; return; }
  if (cp < 127) { o += (char)cp; return; }
  if (cp == 127) return;
  if (cp >= 0xC0 && cp <= 0xFF) { o += LAT[cp - 0xC0]; return; }
  if (cp >= 0x2010 && cp <= 0x2015) { o += '-'; return; }
  switch (cp) {
    case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: case 0x3000: o += ' '; break;
    case 0xAD: case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: break;
    case 0x2018: case 0x2019: case 0x2032: o += '\''; break;
    case 0x201C: case 0x201D: o += '"'; break;
    case 0x2212: o += '-'; break;
    case 0x2026: o += "..."; break;
    case 0x2022: case 0xB7: case 0x25CF: o += '*'; break;
    case 0xA9: o += "(c)"; break;
    case 0xAE: o += "(R)"; break;
    case 0x2122: o += "(TM)"; break;
    case 0xAB: o += "<<"; break;
    case 0xBB: o += ">>"; break;
    case 0x20AC: o += "EUR"; break;
    case 0xA3: o += "GBP"; break;
    case 0xB0: o += "deg"; break;
    case 0x2192: o += "->"; break;
    case 0x2190: o += "<-"; break;
    default: o += '?';
  }
}

struct EntDef { const char* n; const char* v; };
static const EntDef ENTS[] = {
  {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "}, {"ndash", "-"},
  {"mdash", "-"}, {"hellip", "..."}, {"rsquo", "'"}, {"lsquo", "'"}, {"ldquo", "\""}, {"rdquo", "\""},
  {"bull", "*"}, {"middot", "*"}, {"copy", "(c)"}, {"reg", "(R)"}, {"trade", "(TM)"}, {"raquo", ">>"},
  {"laquo", "<<"}, {"times", "x"}, {"deg", "deg"}, {"euro", "EUR"}, {"pound", "GBP"}};

// ---------------------------------------------------------------- page model
// Inline links in line text: \x01 <id hi> <id lo> ...text... \x02   (id = (hi-48)*64 + (lo-48))
static String mk(int id) {
  String s; s += (char)1; s += (char)(48 + (id >> 6)); s += (char)(48 + (id & 63)); return s;
}
static int visLen(const String& s) {
  int n = 0;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == 1) i += 2; else if (c == 2) {} else if (c != ' ') n++;
  }
  return n;
}

static void addLine(char kind, const String& t) {
  if (page.size() >= (size_t)MAX_LINES) { gFull = true; return; }
  int idx = (int)page.size();
  page.push_back(Line{kind, t});
  for (size_t i = 0; i + 2 < t.length(); i++) {
    if (t[i] == 1) {
      int id = ((t[i + 1] - 48) << 6) | (t[i + 2] - 48);
      if (id >= 0 && id < (int)links.size() && links[id].line < 0) links[id].line = idx;
      i += 2;
    }
  }
}
static void addBlank() { if (!page.empty() && page.back().kind != ' ') addLine(' ', ""); }

static bool wrapBlock(char kind, const String& s, const char* first, const char* rest) {
  const int fw = strlen(first), rw = strlen(rest);
  String line = first; int w = fw, open = -1; bool has = false, any = false;
  auto flushLine = [&]() {
    if (open >= 0) line += (char)2;
    addLine(kind, line); any = true;
    line = rest; w = rw; has = false;
    if (open >= 0) line += mk(open);
  };
  size_t i = 0, n = s.length();
  while (i < n && !gFull) {
    while (i < n && s[i] == ' ') i++;
    if (i >= n) break;
    size_t j = i; int wl = 0;
    while (j < n && s[j] != ' ') { if (s[j] == 1) j += 3; else if (s[j] == 2) j++; else { wl++; j++; } }
    if (j > n) j = n;
    if (has && w + 1 + wl > COLS) flushLine();
    if (has) { line += ' '; w++; }
    for (size_t x = i; x < j; x++) {
      char c = s[x];
      if (c == 1 && x + 2 < n) {
        open = ((s[x + 1] - 48) << 6) | (s[x + 2] - 48);
        line += c; line += s[x + 1]; line += s[x + 2]; x += 2;
      } else if (c == 2) { line += c; open = -1; }
      else { if (w >= COLS) flushLine(); line += c; w++; }
    }
    has = true; i = j;
  }
  if (has) flushLine();
  return any;
}

static void chopBlock(const String& s0) {
  String s;
  for (size_t i = 0; i < s0.length();) {
    char c = s0[i];
    if (c == 1) i += 3; else if (c == 2) i++; else { s += c; i++; }
  }
  for (size_t i = 0; i < s.length(); i += COLS) addLine('C', s.substring(i, i + COLS));
}

// kinds: H h1 | h h2-6 | P para | L list | Q quote | C code | R rule | E error
static void emitBlock(char kind, const String& s) {
  switch (kind) {
    case 'R': addLine('R', ""); break;
    case 'H': case 'h': addBlank(); wrapBlock(kind, s, "", ""); break;
    case 'L': wrapBlock('L', s, "* ", "  "); break;
    case 'Q': if (!page.empty() && page.back().kind == 'L') addBlank(); wrapBlock('Q', s, "> ", "> "); break;
    case 'C': chopBlock(s); break;
    case 'E': wrapBlock('E', s, "", ""); addBlank(); break;
    default:
      if (!page.empty() && page.back().kind == 'L') addBlank();
      if (wrapBlock('P', s, "", "")) addBlank();
  }
}

// ---------------------------------------------------------------- HTML / proxy parser
static bool inList(const String& n, const char* list) {
  String k = " " + n + " "; return strstr(list, k.c_str()) != nullptr;
}

static String getAttr(const String& t, const char* n) {
  String lt = t; lt.toLowerCase();
  String key = String(" ") + n; int p = 0; int len = t.length();
  while ((p = lt.indexOf(key, p)) >= 0) {
    int k = p + key.length();
    while (k < len && lt[k] == ' ') k++;
    if (k < len && lt[k] == '=') {
      k++; while (k < len && t[k] == ' ') k++;
      if (k >= len) return "";
      char q = t[k]; String v;
      if (q == '"' || q == '\'') { k++; while (k < len && t[k] != q) v += t[k++]; }
      else { while (k < len && t[k] != ' ') v += t[k++]; }
      return v;
    }
    p = k;
  }
  return "";
}

struct Parser {
  bool proxy = false, plain = false;
  String base, title, ln;
  size_t bytes = 0;
  enum St { TEXT, TAG, COMMENT, RAW } st = TEXT;
  String tag, tail, rawEnd, skipName, cur, ent;
  char q = 0, kind = 'P';
  bool pre = false, inTitle = false, inHead = false, inEnt = false;
  int open = -1, skipDepth = 0, need = 0;
  uint32_t cp = 0;

  void closeLink() { if (open >= 0) { cur += (char)2; open = -1; } }
  void flush() {
    if (visLen(cur) > 0) emitBlock(kind, cur);
    cur = open >= 0 ? mk(open) : String("");
  }

  void textChar(char c) {
    if (skipDepth) return;
    if (inTitle) {
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { if (title.length() && !title.endsWith(" ")) title += ' '; }
      else if (title.length() < 90) title += c;
      return;
    }
    if (inHead) return;
    if (pre) {
      if (c == '\n') flush();
      else if (c == '\r') {}
      else if (c == '\t') cur += "  ";
      else cur += c;
      return;
    }
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && (cur.length() == 0 || cur[cur.length() - 1] == ' ')) return;
    if (cur.length() < 4000) cur += c;
  }

  void flushEnt() {
    if (!inEnt) return;
    inEnt = false; textChar('&');
    for (size_t i = 0; i < ent.length(); i++) textChar(ent[i]);
  }
  void resolveEnt() {
    String o;
    if (ent.length() && ent[0] == '#') {
      uint32_t v = (ent.length() > 1 && (ent[1] == 'x' || ent[1] == 'X')) ? strtoul(ent.c_str() + 2, nullptr, 16)
                                                                          : strtoul(ent.c_str() + 1, nullptr, 10);
      cpAscii(v, o);
    } else {
      bool found = false;
      for (auto& e : ENTS) if (ent == e.n) { o = e.v; found = true; break; }
      if (!found) { o = "&" + ent + ";"; }
    }
    for (size_t i = 0; i < o.length(); i++) textChar(o[i]);
  }

  void handleTag() {
    String t = tag; t.trim();
    if (!t.length()) return;
    bool closing = t[0] == '/';
    String name;
    for (size_t i = closing ? 1 : 0; i < t.length() && isalnum((uint8_t)t[i]); i++) name += (char)tolower(t[i]);
    if (!name.length()) return;
    bool selfClose = t.endsWith("/");
    if (skipDepth) {
      if (name == skipName) { if (closing) skipDepth--; else if (!selfClose) skipDepth++; }
      return;
    }
    if (!closing && !selfClose) {
      if (name == "script" || name == "style") { skipName = name; skipDepth = 1; st = RAW; rawEnd = "</" + name; tail = ""; return; }
      if (inList(name, " svg noscript template iframe select object canvas ")) { skipName = name; skipDepth = 1; return; }
    }
    if (name == "title") { inTitle = !closing; return; }
    if (name == "head") { inHead = !closing; return; }
    if (name == "body") { inHead = false; return; }
    if (inHead) return;

    if (name == "br") { flush(); return; }
    if (name == "hr") { flush(); emitBlock('R', ""); return; }
    if (name == "a") {
      closeLink();
      if (!closing) {
        String h = getAttr(t, "href"); h.replace("&amp;", "&");
        String u = resolveUrl(base, h);
        if (u.length() && (int)links.size() < MAX_LINKS) {
          links.push_back(Link{u, -1}); open = (int)links.size() - 1; cur += mk(open);
        }
      }
      return;
    }
    if (name == "img") {
      if (!closing) {
        String a = getAttr(t, "alt"); a.trim();
        if (a.length()) {
          String s = "[";
          for (size_t i = 0; i < a.length() && i < 40; i++) if (a[i] >= 32 && a[i] < 127) s += a[i];
          s += "] ";
          if (cur.length() && !cur.endsWith(" ")) cur += ' ';
          cur += s;
        }
      }
      return;
    }
    if (name.length() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
      flush(); kind = closing ? 'P' : (name[1] == '1' ? 'H' : 'h'); return;
    }
    if (name == "li") { flush(); kind = closing ? 'P' : 'L'; return; }
    if (name == "blockquote") { flush(); kind = closing ? 'P' : 'Q'; return; }
    if (name == "pre") { flush(); pre = !closing; kind = closing ? 'P' : 'C'; return; }
    if (name == "td" || name == "th") {
      if (!closing && !pre && visLen(cur) > 0) { if (!cur.endsWith(" ")) cur += ' '; cur += "| "; }
      return;
    }
    if (inList(name, " p div section article header footer nav main aside form ul ol dl table thead tbody tfoot tr dd dt figure figcaption center fieldset details summary address caption ")) {
      flush();
      if (name == "ul" || name == "ol" || name == "dl" || name == "table") kind = 'P';
    }
  }

  void procLine() {
    if (!ln.length()) return;
    char k = ln[0]; String v = ln.substring(1);
    switch (k) {
      case 'T': title = v; break;
      case 'B': base = v; break;
      case 'U': if ((int)links.size() < MAX_LINKS) links.push_back(Link{v, -1}); break;
      default: emitBlock(k, v);
    }
  }

  void feed(uint8_t c) {
    if (gFull) return;
    if (++bytes > MAX_BYTES) { gFull = true; return; }
    if (proxy) {
      if (c == '\r') return;
      if (c == '\n') { procLine(); ln = ""; return; }
      if (ln.length() < 1500) ln += (char)c;
      return;
    }
    if (c >= 0x80) {
      if (c >= 0xC0) { need = c >= 0xF0 ? 3 : (c >= 0xE0 ? 2 : 1); cp = c & (0x3F >> need); }
      else if (need > 0) {
        cp = (cp << 6) | (c & 0x3F);
        if (--need == 0 && (st == TEXT || plain)) {
          flushEnt();
          String o; cpAscii(cp, o);
          for (size_t i = 0; i < o.length(); i++) textChar(o[i]);
        }
      }
      return;
    }
    need = 0;
    switch (st) {
      case TEXT:
        if (plain) { textChar((char)c); break; }
        if (c == '<') { flushEnt(); st = TAG; tag = ""; q = 0; }
        else if (c == '&') { flushEnt(); inEnt = true; ent = ""; }
        else if (inEnt) {
          if (c == ';') { resolveEnt(); inEnt = false; }
          else if (isalnum(c) || c == '#') { ent += (char)c; if (ent.length() > 9) flushEnt(); }
          else { flushEnt(); textChar((char)c); }
        } else textChar((char)c);
        break;
      case TAG:
        if (tag.length() == 0 && !q && !(isalpha(c) || c == '/' || c == '!' || c == '?')) {
          st = TEXT; textChar('<'); textChar((char)c); break;   // stray '<' in text
        }
        if (q) { if (c == (uint8_t)q) q = 0; }
        else if (c == '"' || c == '\'') q = (char)c;
        else if (c == '>') { st = TEXT; handleTag(); break; }
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (tag.length() < 700) tag += (char)c;
        if (tag == "!--") { st = COMMENT; tail = ""; }
        break;
      case COMMENT:
        tail += (char)c;
        if (tail.length() > 3) tail = tail.substring(tail.length() - 3);
        if (tail == "-->") st = TEXT;
        break;
      case RAW:
        tail += (char)tolower(c);
        if (tail.length() > rawEnd.length()) tail = tail.substring(1);
        if (tail == rawEnd) { st = TAG; tag = "/" + skipName; q = 0; }
        break;
    }
  }

  void finish() {
    if (proxy) { if (ln.length()) procLine(); ln = ""; }
    else { closeLink(); flush(); }
  }
};
static Parser P;

class PageSink : public Stream {
 public:
  using Print::write;
  uint32_t lastKb = 0;
  size_t write(const uint8_t* b, size_t n) override {
    if (gFull) return 0;
    for (size_t i = 0; i < n && !gFull; i++) P.feed(b[i]);
    uint32_t kb = P.bytes >> 12;
    if (kb != lastKb) { lastKb = kb; status("Loading... " + String((unsigned)(P.bytes / 1024)) + " KB"); }
    return gFull ? 0 : n;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
};

// ---------------------------------------------------------------- network
// Streams the body of `url0` into `sink`. Returns HTTP status or a negative error.
static int httpStream(const String& url0, Stream& sink) {
  String url = url0;
  for (int hop = 0; hop < 6; hop++) {
    bool https = url.startsWith("https://");
    if (!https && !url.startsWith("http://")) return -102;
    WiFiClientSecure sc; WiFiClient pc;
    WiFiClient* cl = https ? (WiFiClient*)&sc : &pc;
    if (https) sc.setInsecure();   // no certificate store on-device; traffic is encrypted but not authenticated
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(12000);
    http.setReuse(false);
    http.setUserAgent(UA);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const char* hk[] = {"Location", "Content-Type"};
    http.collectHeaders(hk, 2);
    if (!http.begin(*cl, url)) return -1;
    http.addHeader("Accept", "text/html,text/plain;q=0.9,*/*;q=0.5");
    http.addHeader("Accept-Language", "en");
    int code = http.GET();
    if (code <= 0) { http.end(); return code; }
    if (code >= 300 && code < 400 && http.hasHeader("Location")) {
      String loc = http.header("Location"); http.end();
      url = resolveUrl(url, loc);
      if (!url.length()) return -101;
      continue;
    }
    if (code >= 200 && code < 300) {
      String ct = http.header("Content-Type"); ct.toLowerCase();
      if (ct.length() && ct.indexOf("text/") < 0 && ct.indexOf("xml") < 0 && ct.indexOf("json") < 0) { http.end(); return -100; }
      if (!P.proxy) {
        P.base = url;
        P.plain = ct.indexOf("text/plain") >= 0 || ct.indexOf("json") >= 0;
        if (P.plain) { P.pre = true; P.kind = 'C'; }
      }
      http.writeToStream(&sink);
    }
    http.end();
    return code;
  }
  return -101;
}

static bool wifiConnect(const String& ssid, const String& pass) {
  WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis(); int d = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250); status("Connecting to " + ssid + String("...........").substring(0, (d++ % 4) + 1));
  }
  return WiFi.status() == WL_CONNECTED;
}

static bool wifiSetup() {
  WiFi.mode(WIFI_STA);
  status("Scanning WiFi...");
  int n = WiFi.scanNetworks();
  std::vector<String> items, ssids; std::vector<bool> isOpen;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false; for (auto& e : ssids) if (e == s) dup = true;
    if (dup) continue;
    bool o = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    ssids.push_back(s); isOpen.push_back(o);
    String label = s.length() > 24 ? s.substring(0, 24) : s;
    items.push_back(label + " " + String(WiFi.RSSI(i)) + (o ? "" : " *"));
  }
  WiFi.scanDelete();
  items.push_back("[Enter SSID manually]");
  int sel = menu("WiFi networks (* = secured)", items);
  if (sel < 0) return false;
  String ssid, pass; bool open = false;
  if (sel < (int)ssids.size()) { ssid = ssids[sel]; open = isOpen[sel]; }
  else if (!editLine("SSID:", ssid)) return false;
  if (!ssid.length()) return false;
  if (!open) {
    pass = (ssid == cfgSsid) ? cfgPass : String("");
    if (!editLine("Password for " + ssid, pass)) return false;
  }
  if (wifiConnect(ssid, pass)) {
    cfgSsid = ssid; cfgPass = pass;
    prefs.putString("ssid", ssid); prefs.putString("pass", pass);
    status("Connected: " + WiFi.localIP().toString()); delay(900);
    return true;
  }
  status("WiFi connect failed (wrong password?)"); delay(1500);
  return false;
}

static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (cfgSsid.length() && wifiConnect(cfgSsid, cfgPass)) return true;
  return wifiSetup();
}

// ---------------------------------------------------------------- bookmarks
static std::vector<String> loadBm() {
  std::vector<String> v; String s = prefs.getString("bm", ""); int i = 0;
  while (i < (int)s.length()) {
    int j = s.indexOf('\n', i); if (j < 0) j = s.length();
    if (j > i) v.push_back(s.substring(i, j));
    i = j + 1;
  }
  return v;
}
static void saveBm(const std::vector<String>& v) {
  String s; for (auto& e : v) { s += e; s += '\n'; }
  prefs.putString("bm", s);
}

// ---------------------------------------------------------------- drawing
static uint16_t kindColor(char k) {
  switch (k) {
    case 'H': return C_YELLOW; case 'h': return C_ORANGE; case 'Q': return C_LGREY;
    case 'C': return C_GREEN; case 'E': return C_RED; default: return C_WHITE;
  }
}

static void drawLine(const Line& L, int y) {
  if (L.kind == 'R') { GFX.drawFastHLine(4, y + 4, 232, C_GREY); return; }
  uint16_t fg = kindColor(L.kind);
  int x = 0, id = -1; String seg;
  auto put = [&]() {
    if (!seg.length()) return;
    bool sel = id >= 0 && id == selLink;
    uint16_t f = id >= 0 ? (sel ? C_BLACK : C_CYAN) : fg;
    uint16_t b = sel ? C_CYAN : C_BLACK;
    GFX.setTextColor(f, b); GFX.setCursor(x, y); GFX.print(seg.c_str());
    x += seg.length() * 6; seg = "";
  };
  for (size_t i = 0; i < L.t.length();) {
    char c = L.t[i];
    if (c == 1 && i + 2 < L.t.length()) { put(); id = ((L.t[i + 1] - 48) << 6) | (L.t[i + 2] - 48); i += 3; }
    else if (c == 2) { put(); id = -1; i++; }
    else { seg += c; i++; }
  }
  put();
}

static void drawPage() {
  GFX.startWrite();
  bar(0, TOP_H, C_NAVY, pageTitle.length() ? pageTitle : curUrl);
  for (int r = 0; r < VIS; r++) {
    int li = scrollTop + r, y = Y0 + r * LINE_H;
    GFX.fillRect(0, y, 240, LINE_H, C_BLACK);
    if (li < (int)page.size()) drawLine(page[li], y);
  }
  String s;
  if (selLink >= 0 && selLink < (int)links.size()) s = links[selLink].url;
  else {
    int pct = page.size() <= (size_t)VIS ? 100 : (int)((scrollTop + VIS) * 100 / (int)page.size());
    s = String(cfgProxyMode ? "PX " : "DR ") + pct + "% bat" + (int)M5.Power.getBatteryLevel() + "% h=help";
  }
  status(s);
  GFX.endWrite();
}

static bool linkVisible(int id) { int l = links[id].line; return l >= scrollTop && l < scrollTop + VIS; }

static void scrollTo(int t) {
  int mx = (int)page.size() > VIS ? (int)page.size() - VIS : 0;
  if (t < 0) t = 0; if (t > mx) t = mx;
  scrollTop = t;
  if (selLink >= 0 && selLink < (int)links.size() && !linkVisible(selLink)) selLink = -1;
  drawPage();
}

static void moveLink(int dir) {
  int n = links.size();
  if (!n) { status("No links on this page"); return; }
  int id = selLink;
  if (id < 0 || id >= n || !linkVisible(id)) {
    id = -1;
    if (dir > 0) { for (int i = 0; i < n; i++) if (links[i].line >= scrollTop && links[i].line < scrollTop + VIS) { id = i; break; } }
    else { for (int i = n - 1; i >= 0; i--) if (links[i].line >= scrollTop && links[i].line < scrollTop + VIS) { id = i; break; } }
    if (id < 0) {
      if (dir > 0) { for (int i = 0; i < n; i++) if (links[i].line >= scrollTop) { id = i; break; } }
      else { for (int i = n - 1; i >= 0; i--) if (links[i].line >= 0 && links[i].line < scrollTop) { id = i; break; } }
    }
    if (id < 0) { status("No more links"); return; }
  } else {
    int i = id + dir;
    while (i >= 0 && i < n && links[i].line < 0) i += dir;
    if (i < 0 || i >= n) { status("No more links"); return; }
    id = i;
  }
  selLink = id;
  int l = links[id].line;
  if (l < scrollTop || l >= scrollTop + VIS) {
    int mx = (int)page.size() > VIS ? (int)page.size() - VIS : 0;
    scrollTop = l - VIS / 2; if (scrollTop < 0) scrollTop = 0; if (scrollTop > mx) scrollTop = mx;
  }
  drawPage();
}

// ---------------------------------------------------------------- built-in pages
static const char* HELP_HTML = R"HTML(
<h1>Help</h1>
<h2>Reading</h2>
<ul><li>; and . : scroll up / down (hold to repeat)</li>
<li>Space / p : page down / up</li>
<li>Tab or / : next link, comma : previous link</li>
<li>Enter : open the highlighted link</li></ul>
<h2>Navigation</h2>
<ul><li>g : type a URL or a search</li>
<li>e : edit the current URL</li>
<li>b or Del : back, f : forward, r : reload</li>
<li>o : home page</li>
<li>d : bookmark page, k : bookmark list</li></ul>
<h2>System</h2>
<ul><li>w : WiFi setup</li>
<li>m : switch Proxy / Direct mode</li>
<li>s : settings (proxy URL, key, home)</li>
<li>In text boxes: Enter ok, Tab clear, Fn+` cancel</li></ul>
<h2>Notes</h2>
<p>No images, JavaScript or forms. Direct mode works for most small pages; use proxy mode for big or heavy sites.</p>
)HTML";

static const char* HOME_HTML = R"HTML(
<h1>Cardputer Browser</h1>
<p>g = URL or search, h = help, w = WiFi, s = settings, m = proxy/direct.</p>
<h2>Quick links</h2>
<ul>
<li><a href="https://text.npr.org/">NPR (text only)</a></li>
<li><a href="https://lite.cnn.com/">CNN Lite</a></li>
<li><a href="https://news.ycombinator.com/">Hacker News</a></li>
<li><a href="https://en.m.wikipedia.org/wiki/Special:Random">Random Wikipedia article</a></li>
<li><a href="https://lite.duckduckgo.com/lite/">DuckDuckGo Lite</a></li>
<li><a href="http://example.com/">example.com</a></li>
</ul>
)HTML";

static void feedStr(const String& s) { for (size_t i = 0; i < s.length(); i++) P.feed((uint8_t)s[i]); }

static void renderAbout(const String& url) {
  P.base = "https://about.invalid/";
  if (url == "about:help") { pageTitle = "Help"; feedStr(HELP_HTML); }
  else {
    pageTitle = "Home"; feedStr(HOME_HTML);
    auto bm = loadBm();
    if (!bm.empty()) {
      feedStr("<h2>Bookmarks</h2><ul>");
      for (auto& e : bm) {
        int sp = e.indexOf(' ');
        String u = sp > 0 ? e.substring(0, sp) : e, t = sp > 0 ? e.substring(sp + 1) : e;
        feedStr("<li><a href=\"" + u + "\">" + t + "</a></li>");
      }
      feedStr("</ul>");
    }
  }
  P.finish();
}

// ---------------------------------------------------------------- loading
static void resetPage() {
  std::vector<Line>().swap(page);
  std::vector<Link>().swap(links);
  P = Parser(); gFull = false; selLink = -1; scrollTop = 0;
}

static void errorPage(const String& head, const String& msg) {
  pageTitle = "Error";
  addLine('E', head); addBlank();
  wrapBlock('P', msg, "", "");
}

static void loadPage(String url) {
  resetPage();
  curUrl = url; pageTitle = "";
  if (url.startsWith("about:")) { renderAbout(url); drawPage(); return; }
  if (!ensureWifi()) { errorPage("No WiFi", "Press w to set up WiFi."); drawPage(); return; }
  status("Loading...");
  PageSink sink; int code;
  P.base = url;
  if (cfgProxyMode) {
    if (!cfgProxy.length()) {
      errorPage("Proxy not set", "Press s > Proxy to enter your Wasmer URL, or press m to use direct mode.");
      drawPage(); return;
    }
    P.proxy = true;
    String pu = cfgProxy + "/r?u=" + urlEncode(url);
    if (cfgKey.length()) pu += "&k=" + urlEncode(cfgKey);
    code = httpStream(pu, sink);
  } else {
    code = httpStream(url, sink);
  }
  if (code >= 200 && code < 300) {
    P.finish();
    pageTitle = P.title;
    if (P.base.length()) curUrl = P.base;
    if (page.empty()) addLine('P', "(empty page)");
    if (gFull) page.push_back(Line{'E', "-- page truncated --"});
  } else {
    String msg;
    if (code == -100) msg = "Unsupported content type (images, PDFs, downloads are not supported).";
    else if (code == -101) msg = "Too many redirects.";
    else if (code == -102) msg = "Unsupported URL scheme.";
    else if (code > 0) msg = "Server returned HTTP " + String(code);
    else msg = HTTPClient::errorToString(code) + " (code " + String(code) + ")";
    errorPage("Could not load page", url + "  --  " + msg);
  }
  drawPage();
}

static void navigate(const String& url) {
  if (histPos >= 0 && histPos < (int)histScroll.size()) histScroll[histPos] = scrollTop;
  loadPage(url);
  history.resize(histPos + 1); histScroll.resize(histPos + 1);
  history.push_back(url); histScroll.push_back(0); histPos++;
  if (history.size() > 30) { history.erase(history.begin()); histScroll.erase(histScroll.begin()); histPos--; }
}

static void goHist(int pos) {
  if (pos < 0 || pos >= (int)history.size()) { status("No more history"); return; }
  histScroll[histPos] = scrollTop;
  histPos = pos;
  loadPage(history[histPos]);
  scrollTo(histScroll[histPos]);
}

static void bookmarksMenu() {
  while (true) {
    auto bm = loadBm();
    std::vector<String> items;
    for (auto& e : bm) { int sp = e.indexOf(' '); items.push_back(sp > 0 ? e.substring(sp + 1) : e); }
    int s = menu("Bookmarks", items, 0, true);
    if (s >= 1000) { bm.erase(bm.begin() + (s - 1000)); saveBm(bm); continue; }
    if (s < 0) { drawPage(); return; }
    int sp = bm[s].indexOf(' ');
    navigate(sp > 0 ? bm[s].substring(0, sp) : bm[s]);
    return;
  }
}

static void settingsMenu() {
  while (true) {
    std::vector<String> it = {
      String("Mode: ") + (cfgProxyMode ? "PROXY" : "DIRECT"),
      "Proxy URL: " + (cfgProxy.length() ? cfgProxy : String("(not set)")),
      "Proxy key: " + (cfgKey.length() ? String("(set)") : String("(none)")),
      "Home page: " + cfgHome,
      "WiFi: " + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("(not connected)")),
      "Clear all bookmarks",
      "Back"};
    int s = menu("Settings", it);
    if (s < 0 || s == 6) { drawPage(); return; }
    String v;
    switch (s) {
      case 0: cfgProxyMode = !cfgProxyMode; prefs.putBool("pm", cfgProxyMode); break;
      case 1:
        v = cfgProxy;
        if (editLine("Proxy URL e.g. https://x.wasmer.app", v)) {
          v.trim(); while (v.endsWith("/")) v.remove(v.length() - 1);
          if (v.length() && v.indexOf("://") < 0) v = "https://" + v;
          cfgProxy = v; prefs.putString("proxy", v);
        }
        break;
      case 2: v = cfgKey; if (editLine("Proxy key (blank = none)", v)) { v.trim(); cfgKey = v; prefs.putString("key", v); } break;
      case 3: v = cfgHome; if (editLine("Home page URL", v)) { v.trim(); if (!v.length()) v = "about:home"; cfgHome = v; prefs.putString("home", v); } break;
      case 4: wifiSetup(); break;
      case 5: prefs.putString("bm", ""); break;
    }
  }
}

// ---------------------------------------------------------------- key handling
static void doKey(const Key& k) {
  if (k.enter) {
    if (selLink >= 0 && selLink < (int)links.size()) navigate(links[selLink].url);
    else status("Pick a link first: Tab or , /");
    return;
  }
  if (k.del) { goHist(histPos - 1); return; }
  if (k.tab) { moveLink(+1); return; }
  char c = k.c; if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
  String v;
  switch (c) {
    case ';': scrollTo(scrollTop - 1); break;
    case '.': scrollTo(scrollTop + 1); break;
    case ' ': scrollTo(scrollTop + VIS - 1); break;
    case 'p': scrollTo(scrollTop - (VIS - 1)); break;
    case ',': moveLink(-1); break;
    case '/': moveLink(+1); break;
    case 'g':
      if (editLine("Go to URL or search:", v)) { v = normalizeInput(v); if (v.length()) navigate(v); }
      else drawPage();
      break;
    case 'e':
      v = curUrl;
      if (editLine("Edit URL:", v)) { v = normalizeInput(v); if (v.length()) navigate(v); }
      else drawPage();
      break;
    case 'b': goHist(histPos - 1); break;
    case 'f': goHist(histPos + 1); break;
    case 'r': { int s = scrollTop; loadPage(curUrl); scrollTo(s); } break;
    case 'o': navigate(cfgHome); break;
    case 'h': navigate("about:help"); break;
    case 'm':
      cfgProxyMode = !cfgProxyMode; prefs.putBool("pm", cfgProxyMode);
      status(cfgProxyMode ? "Mode: PROXY (reloading)" : "Mode: DIRECT (reloading)");
      delay(400); loadPage(curUrl);
      break;
    case 'w': wifiSetup(); drawPage(); break;
    case 's': settingsMenu(); break;
    case 'k': bookmarksMenu(); break;
    case 'd': {
      if (curUrl.startsWith("about:")) { status("Can't bookmark this page"); break; }
      auto bm = loadBm();
      String t = pageTitle.length() ? pageTitle : curUrl; if (t.length() > 40) t = t.substring(0, 40);
      bool dup = false; for (auto& e : bm) if (e.startsWith(curUrl + " ")) dup = true;
      if (!dup) { bm.push_back(curUrl + " " + t); saveBm(bm); }
      status(dup ? "Already bookmarked" : "Bookmarked");
    } break;
  }
}

// ---------------------------------------------------------------- Arduino entry points
void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  GFX.setRotation(1);
  GFX.setTextFont(1); GFX.setTextSize(1); GFX.setTextWrap(false);
  GFX.fillScreen(C_BLACK);
  GFX.setBrightness(110);

  prefs.begin("cpbrowser", false);
  cfgSsid = prefs.getString("ssid", ""); cfgPass = prefs.getString("pass", "");
  cfgProxy = prefs.getString("proxy", ""); cfgKey = prefs.getString("key", "");
  cfgHome = prefs.getString("home", "about:home"); cfgProxyMode = prefs.getBool("pm", false);

  GFX.setTextColor(C_CYAN, C_BLACK); GFX.setCursor(60, 50); GFX.print("Cardputer Browser");
  GFX.setTextColor(C_WHITE, C_BLACK); GFX.setCursor(40, 66); GFX.print("press h for help once loaded");

  if (cfgSsid.length()) wifiConnect(cfgSsid, cfgPass);
  else wifiSetup();
  navigate(cfgHome);
}

void loop() {
  Key k;
  if (getKey(k)) doKey(k); else delay(10);
}
