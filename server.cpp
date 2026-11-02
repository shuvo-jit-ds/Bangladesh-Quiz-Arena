// ====================================================================
//  Bangladesh Quiz Arena  -  WEB VERSION  (C++ OOP Mini Project)
//
//  ALL game logic is written in C++ (this file). The browser page
//  (web/index.html) only shows buttons and screens and asks this
//  C++ server for everything through small API calls.
//
//  Compile (Windows / MinGW):  g++ -std=c++17 server.cpp -o server -lws2_32 -lwsock32
//  Compile (Linux / Mac)    :  g++ -std=c++17 server.cpp -o server -pthread
//  Run                      :  server        then open  http://localhost:8080
// ====================================================================
#include "httplib.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif
using namespace std;

// ------------------------------------------------------------------
//  Small helper functions
// ------------------------------------------------------------------
static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static string lower(string s) {
    for (size_t i = 0; i < s.size(); i++) s[i] = (char)tolower((unsigned char)s[i]);
    return s;
}
static vector<string> split(const string& s, char d) {
    vector<string> r;
    string part;
    stringstream ss(s);
    while (getline(ss, part, d)) r.push_back(part);
    return r;
}
// make a text safe for our '|' separated files and for display
static string cleanField(string s, size_t maxLen = 80) {
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '|' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t') s[i] = ' ';
    s = trim(s);
    if (s.size() > maxLen) s.resize(maxLen);
    return s;
}
static int toInt(const string& s, int def = 0) {
    try { return stoi(s); } catch (...) { return def; }
}
// ---- tiny JSON writer (no external library) ----
static string jesc(const string& s) {
    string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}
static string q(const string& s) { return "\"" + jesc(s) + "\""; }
static string errJson(const string& m) { return "{\"ok\":false,\"error\":" + q(m) + "}"; }
static string strArray(const vector<string>& v) {
    string o = "[";
    for (size_t i = 0; i < v.size(); i++) { if (i) o += ","; o += q(v[i]); }
    return o + "]";
}
static void makeDir(const string& d) {
#ifdef _WIN32
    _mkdir(d.c_str());
#else
    mkdir(d.c_str(), 0755);
#endif
}
static string readWholeFile(const string& f) {
    ifstream in(f.c_str(), ios::binary);
    stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
// local calendar day number (used for streaks) and local hour
static long localDay() {
    time_t t = time(0);
    tm d = *localtime(&t);
    d.tm_hour = 12; d.tm_min = 0; d.tm_sec = 0; d.tm_isdst = -1;
    return (long)(mktime(&d) / 86400);
}
static int localHour() {
    time_t t = time(0);
    return localtime(&t)->tm_hour;
}

// ---- reading question rows from a pasted/uploaded table (CSV, Excel-paste TSV, or pipe format) ----
// split one CSV line; understands "quoted, fields" and "" inside quotes
static vector<string> csvSplit(const string& line, char delim) {
    vector<string> out;
    string cur;
    bool inQ = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (inQ) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; i++; }
                else inQ = false;
            } else cur += c;
        } else if (c == '"') inQ = true;
        else if (c == delim) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}
// pick the separator used in this line and split it into columns
static vector<string> splitRow(const string& line) {
    if (line.find('|') != string::npos) {
        vector<string> f;
        size_t start = 0;
        for (;;) {                                   // keep empty columns too
            size_t p = line.find('|', start);
            if (p == string::npos) { f.push_back(line.substr(start)); break; }
            f.push_back(line.substr(start, p - start));
            start = p + 1;
        }
        return f;
    }
    if (line.find('\t') != string::npos) return csvSplit(line, '\t');
    size_t commas = count(line.begin(), line.end(), ','), semis = count(line.begin(), line.end(), ';');
    return csvSplit(line, (semis >= 6 && commas < 6) ? ';' : ',');
}
static string flatten(string s, size_t maxLen) {        // one line, no '|' (our file separator)
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '|') s[i] = '/';
        else if (s[i] == '\n' || s[i] == '\r' || s[i] == '\t') s[i] = ' ';
    s = trim(s);
    if (s.size() > maxLen) s.resize(maxLen);
    return s;
}

// ------------------------------------------------------------------
//  Question class
// ------------------------------------------------------------------
class Question {
public:
    string subject, text, opt[4];
    int ans;                       // 0..3
    Question() : ans(0) {}
};

// ------------------------------------------------------------------
//  Player class (one registered user, saved in players.txt)
// ------------------------------------------------------------------
class Player {
public:
    string name;
    int district, institution, streak, shields;
    long lastDay, dayOffset;
    int stamps, totalScore;
    string examName, examSubject;
    int examDays;
    int coins, earned, totalEarned, pointsSpent;
    int answered, correctTotal, roundsPlayed, bestScore;      // statistics

    Player() : district(0), institution(0), streak(0), shields(0), lastDay(0), dayOffset(0),
               stamps(0), totalScore(0), examDays(0), coins(0), earned(0), totalEarned(0), pointsSpent(0),
               answered(0), correctTotal(0), roundsPlayed(0), bestScore(0) {}

    string toLine() const {
        ostringstream o;
        o << name << "|" << district << "|" << institution << "|" << streak << "|" << shields << "|"
          << lastDay << "|" << dayOffset << "|" << stamps << "|" << totalScore << "|"
          << examName << "|" << examSubject << "|" << examDays << "|" << coins << "|"
          << earned << "|" << totalEarned << "|" << pointsSpent << "|"
          << answered << "|" << correctTotal << "|" << roundsPlayed << "|" << bestScore;
        return o.str();
    }
    bool fromLine(const string& line) {
        vector<string> f = split(line, '|');
        if (f.size() < 16) return false;
        name = f[0];
        district = toInt(f[1]); institution = toInt(f[2]); streak = toInt(f[3]); shields = toInt(f[4]);
        lastDay = toInt(f[5]); dayOffset = toInt(f[6]); stamps = toInt(f[7]); totalScore = toInt(f[8]);
        examName = f[9]; examSubject = f[10]; examDays = toInt(f[11]);
        coins = toInt(f[12]); earned = toInt(f[13]); totalEarned = toInt(f[14]); pointsSpent = toInt(f[15]);
        if (f.size() >= 20) {      // statistics were added later; older save files do not have them
            answered = toInt(f[16]); correctTotal = toInt(f[17]); roundsPlayed = toInt(f[18]); bestScore = toInt(f[19]);
        }
        return !name.empty();
    }
};

// ------------------------------------------------------------------
//  PlayerStore class: all players, loaded/saved from players.txt
// ------------------------------------------------------------------
class PlayerStore {
    map<string, Player> players;   // key = lowercase name
    string file;
public:
    PlayerStore() : file("players.txt") {
        ifstream in(file.c_str());
        string line;
        while (getline(in, line)) { Player p; if (p.fromLine(line)) players[lower(p.name)] = p; }
    }
    Player* find(const string& name) {
        map<string, Player>::iterator it = players.find(lower(trim(name)));
        return it == players.end() ? 0 : &it->second;
    }
    Player* create(const string& name, int district, int institution) {
        Player p; p.name = name; p.district = district; p.institution = institution;
        players[lower(name)] = p;
        save();
        return &players[lower(name)];
    }
    void save() const {
        ofstream out(file.c_str());
        for (map<string, Player>::const_iterator it = players.begin(); it != players.end(); ++it)
            out << it->second.toLine() << "\n";
    }
};

// ------------------------------------------------------------------
//  ScoreBoard class: District War and School/College Championship
// ------------------------------------------------------------------
class ScoreBoard {
    string file;
    vector<string> names;
    map<string, int> pts;
public:
    ScoreBoard(const string& f, const vector<string>& n) : file(f), names(n) {
        for (size_t i = 0; i < names.size(); i++) pts[names[i]] = 0;
        ifstream in(file.c_str());
        string line;
        while (getline(in, line)) {
            vector<string> p = split(line, '|');
            if (p.size() == 2 && pts.count(p[0])) pts[p[0]] = toInt(p[1]);
        }
    }
    void add(int index, int p) {
        if (index < 0 || index >= (int)names.size()) return;
        pts[names[index]] += p;
        ofstream out(file.c_str());
        for (size_t i = 0; i < names.size(); i++) out << names[i] << "|" << pts[names[i]] << "\n";
    }
    const vector<string>& getNames() const { return names; }
    string toJson() const {
        vector<pair<int, string> > v;
        for (size_t i = 0; i < names.size(); i++) v.push_back(make_pair(pts.at(names[i]), names[i]));
        sort(v.rbegin(), v.rend());
        string o = "[";
        for (size_t i = 0; i < v.size(); i++) {
            if (i) o += ",";
            o += "{\"name\":" + q(v[i].second) + ",\"pts\":" + to_string(v[i].first) + "}";
        }
        return o + "]";
    }
};

// ------------------------------------------------------------------
//  TopScores class: Beat the Toppers (best 3 rounds ever)
// ------------------------------------------------------------------
class TopScores {
    string file;
    vector<pair<int, string> > list;
public:
    TopScores() : file("toppers.txt") {
        ifstream in(file.c_str());
        string line;
        while (getline(in, line)) {
            vector<string> p = split(line, '|');
            if (p.size() == 2) list.push_back(make_pair(toInt(p[0]), p[1]));
        }
        sort(list.rbegin(), list.rend());
    }
    void add(int score, const string& name) {
        list.push_back(make_pair(score, name));
        sort(list.rbegin(), list.rend());
        if (list.size() > 3) list.resize(3);
        ofstream out(file.c_str());
        for (size_t i = 0; i < list.size(); i++) out << list[i].first << "|" << list[i].second << "\n";
    }
    int best() const { return list.empty() ? 0 : list[0].first; }
    string toJson() const {
        string o = "[";
        for (size_t i = 0; i < list.size(); i++) {
            if (i) o += ",";
            o += "{\"name\":" + q(list[i].second) + ",\"score\":" + to_string(list[i].first) + "}";
        }
        return o + "]";
    }
};

// ------------------------------------------------------------------
//  Note class + Marketplace class (Notes Marketplace feature)
// ------------------------------------------------------------------
class Note {
public:
    int id, price, sales;
    string seller, title, subject;
    bool demo;                       // demo notes have a generated picture instead of an upload
    Note() : id(0), price(0), sales(0), demo(false) {}
    string toLine() const {
        ostringstream o;
        o << id << "|" << seller << "|" << title << "|" << subject << "|" << price << "|" << sales << "|" << (demo ? 1 : 0);
        return o.str();
    }
    bool fromLine(const string& line) {
        vector<string> f = split(line, '|');
        if (f.size() < 7) return false;
        id = toInt(f[0]); seller = f[1]; title = f[2]; subject = f[3];
        price = toInt(f[4]); sales = toInt(f[5]); demo = (f[6] == "1");
        return id > 0;
    }
};

class Marketplace {
    vector<Note> notes;
    string file;
public:
    Marketplace() : file("market.txt") {
        makeDir("uploads");
        ifstream in(file.c_str());
        string line;
        while (getline(in, line)) { Note n; if (n.fromLine(line)) notes.push_back(n); }
        if (notes.empty()) {         // starter notes from "other students"
            addNote("Anika", "Physics Chapter 3 - Motion", "Physics", 20, true);
            addNote("Rahim", "Math Formula Sheet", "Math", 15, true);
            addNote("Mitu", "Bangladesh Affairs Summary", "Bangladesh", 10, true);
        }
    }
    void save() const {
        ofstream out(file.c_str());
        for (size_t i = 0; i < notes.size(); i++) out << notes[i].toLine() << "\n";
    }
    int addNote(const string& seller, const string& title, const string& subject, int price, bool demo) {
        Note n;
        int m = 0;
        for (size_t i = 0; i < notes.size(); i++) m = max(m, notes[i].id);
        n.id = m + 1; n.seller = seller; n.title = title; n.subject = subject; n.price = price; n.demo = demo;
        notes.push_back(n);
        save();
        return n.id;
    }
    Note* find(int id) {
        for (size_t i = 0; i < notes.size(); i++) if (notes[i].id == id) return &notes[i];
        return 0;
    }
    vector<Note>& all() { return notes; }
    int salesOf(const string& seller) const {
        int s = 0;
        for (size_t i = 0; i < notes.size(); i++) if (lower(notes[i].seller) == lower(seller)) s += notes[i].sales;
        return s;
    }
    // ---- purchases (purchases.txt: "player|noteId") ----
    bool hasBought(const string& player, int id) const {
        ifstream in("purchases.txt");
        string line, key = lower(player) + "|" + to_string(id);
        while (getline(in, line)) if (trim(line) == key) return true;
        return false;
    }
    void recordPurchase(const string& player, int id) const {
        ofstream out("purchases.txt", ios::app);
        out << lower(player) << "|" << id << "\n";
    }
    // ---- image storage (the picture is stored as a text "data URL") ----
    static string imagePath(int id) { return "uploads/note_" + to_string(id) + ".txt"; }
    void saveImage(int id, const string& dataUrl) const {
        ofstream out(imagePath(id).c_str(), ios::binary);
        out << dataUrl;
    }
    string loadImage(const Note& n) const {
        if (n.demo) {   // generated "handwritten" picture for starter notes
            string s = "<svg xmlns='http://www.w3.org/2000/svg' width='600' height='400'>"
                       "<rect width='600' height='400' fill='%23fffdf2'/>";
            for (int y = 70; y < 400; y += 36)
                s += "<line x1='30' y1='" + to_string(y) + "' x2='570' y2='" + to_string(y) + "' stroke='%23c9d8f0'/>";
            s += "<line x1='60' y1='0' x2='60' y2='400' stroke='%23e8a0a0'/>"
                 "<text x='80' y='55' font-family='cursive' font-size='28' fill='%231a3a8a'>";
            string t = n.title;
            for (size_t i = 0; i < t.size(); i++) if (t[i] == '<' || t[i] == '&' || t[i] == '\'') t[i] = ' ';
            s += t + "</text><text x='80' y='130' font-family='cursive' font-size='22' fill='%231a3a8a'>"
                 "Demo note by " + n.seller + "</text>"
                 "<text x='80' y='166' font-family='cursive' font-size='22' fill='%231a3a8a'>(students upload real photos)</text></svg>";
            return "data:image/svg+xml;utf8," + s;
        }
        return readWholeFile(imagePath(n.id));
    }
};

// ------------------------------------------------------------------
//  Round class: one quiz round in progress
// ------------------------------------------------------------------
class Round {
public:
    string token, player, mode, title;
    vector<int> idx;       // indexes into the question bank
    vector<int> answers;   // player's answers (-1 = not answered)
};

// ------------------------------------------------------------------
//  QuizEngine class: the brain of the game (all features live here)
// ------------------------------------------------------------------
class QuizEngine {
public:
    vector<Question> bank;
    PlayerStore store;
    ScoreBoard districts, institutions;
    TopScores toppers;
    Marketplace market;
    vector<string> places;
    map<string, Round> rounds;                  // token -> round
    map<string, vector<string> > wrongBook;     // player -> list of question texts
    bool forceDouble;
    mt19937 rng;
    string adminPin;
    int adminFails;

    static vector<string> mkDistricts() {
        const char* d[] = {"Dhaka", "Chattogram", "Sylhet", "Rajshahi", "Khulna"};
        return vector<string>(d, d + 5);
    }
    static vector<string> mkInstitutions() {
        const char* d[] = {"Dhaka College", "Notre Dame College", "Rajshahi College"};
        return vector<string>(d, d + 3);
    }

    QuizEngine()
        : districts("districts.txt", mkDistricts()), institutions("institutions.txt", mkInstitutions()),
          forceDouble(false), rng((unsigned)time(0)), adminFails(0) {
        const char* pl[] = {"Dhaka", "Cox's Bazar", "Padma Bridge", "Sylhet", "Sundarbans"};
        places.assign(pl, pl + 5);
        loadAdminPin();
        loadQuestions("questions.txt");
        loadWrongBook();
    }

    // ---------- question file: Subject|Question|Opt1|Opt2|Opt3|Opt4|Correct(1-4) ----------
    // Result of reading a block of text that contains question rows
    struct ImportResult {
        vector<Question> good;
        vector<string> errors;     // "Line 4: ..." (first few)
        int invalid;
        ImportResult() : invalid(0) {}
    };
    // existing subject names keep their spelling ("physics" -> "Physics")
    string normSubject(const string& s, const vector<Question>& known) const {
        for (size_t i = 0; i < known.size(); i++) if (lower(known[i].subject) == lower(s)) return known[i].subject;
        for (size_t i = 0; i < bank.size(); i++) if (lower(bank[i].subject) == lower(s)) return bank[i].subject;
        return s;
    }
    // Reads rows like:  Subject | Question | Opt1 | Opt2 | Opt3 | Opt4 | Correct(1-4 or A-D)
    // (pipe, tab or comma separated; first row may be a header)
    ImportResult parseRows(const string& text) const {
        ImportResult R;
        stringstream ss(text);
        string line;
        int lineNo = 0;
        while (getline(ss, line)) {
            lineNo++;
            if (lineNo == 1 && line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB) line = line.substr(3);
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            vector<string> f = splitRow(line);
            while (!f.empty() && trim(f.back()).empty() && f.size() > 7) f.pop_back();   // Excel may add empty columns
            for (size_t i = 0; i < f.size(); i++) f[i] = trim(f[i]);
            string why;
            if (f.size() != 7) why = "needs 7 columns but found " + to_string(f.size());
            else {
                string c = lower(f[6]);
                int a = 0;
                if (c == "a") a = 1; else if (c == "b") a = 2; else if (c == "c") a = 3; else if (c == "d") a = 4;
                else a = toInt(c, 0);
                if (a < 1 || a > 4) {
                    if (lower(f[0]) == "subject" || lower(f[0]) == "category") continue;     // header row: skip quietly
                    why = "correct answer must be 1, 2, 3 or 4";
                } else if (f[0].empty() || f[1].empty()) why = "subject and question cannot be empty";
                else if (f[2].empty() || f[3].empty() || f[4].empty() || f[5].empty()) why = "all four options are needed";
                else {
                    Question Q;
                    Q.subject = normSubject(flatten(f[0], 30), R.good);
                    Q.text = flatten(f[1], 300);
                    for (int i = 0; i < 4; i++) Q.opt[i] = flatten(f[2 + i], 120);
                    Q.ans = a - 1;
                    R.good.push_back(Q);
                    continue;
                }
            }
            R.invalid++;
            if (R.errors.size() < 10) R.errors.push_back("Line " + to_string(lineNo) + ": " + why);
        }
        return R;
    }
    // write the question bank to questions.txt (the format the game reads at startup)
    void saveQuestions() const {
        ofstream out("questions.txt");
        out << "# Question bank - written by the Admin page. Format:\n"
            << "# Subject|Question|Option1|Option2|Option3|Option4|CorrectOption(1-4)\n";
        string last;
        for (size_t i = 0; i < bank.size(); i++) {
            if (bank[i].subject != last) { out << "\n# ---- " << bank[i].subject << " ----\n"; last = bank[i].subject; }
            const Question& Q = bank[i];
            out << Q.subject << "|" << Q.text << "|" << Q.opt[0] << "|" << Q.opt[1] << "|" << Q.opt[2] << "|" << Q.opt[3] << "|" << (Q.ans + 1) << "\n";
        }
    }
    void loadQuestions(const string& fname) {
        string text = readWholeFile(fname);
        ImportResult R = parseRows(text);
        for (size_t i = 0; i < R.errors.size(); i++) cout << "Warning: " << fname << " " << R.errors[i] << " (skipped)\n";
        bank = R.good;
        if (bank.empty()) { cout << "ERROR: no questions loaded. Put questions.txt next to the server (or import from the Admin page).\n"; }
        else cout << "Loaded " << bank.size() << " questions from " << fname << ".\n";
    }
    // ---------- Admin: import / add / delete / list ----------
    void loadAdminPin() {
        const char* env = getenv("ADMIN_PIN");
        if (env && trim(env).size() >= 4) { adminPin = trim(env); return; }
        string f = trim(readWholeFile("admin_pin.txt"));
        if (f.size() >= 4) { adminPin = f; return; }
        adminPin = "admin1234";
    }
    bool pinIsDefault() const { return adminPin == "admin1234"; }
    // returns "" when the PIN is right, otherwise an error message
    string checkPin(const string& pin) {
        if (adminFails >= 8) return "Too many wrong PIN attempts. Restart the server to try again.";
        if (pin != adminPin) { adminFails++; return "Wrong admin PIN."; }
        adminFails = 0;
        return "";
    }
    string subjectCountsJson() const {
        vector<string> names; vector<int> counts;
        for (size_t i = 0; i < bank.size(); i++) {
            size_t k = 0;
            while (k < names.size() && names[k] != bank[i].subject) k++;
            if (k == names.size()) { names.push_back(bank[i].subject); counts.push_back(0); }
            counts[k]++;
        }
        string o = "[";
        for (size_t i = 0; i < names.size(); i++) {
            if (i) o += ",";
            o += "{\"subject\":" + q(names[i]) + ",\"count\":" + to_string(counts[i]) + "}";
        }
        return o + "]";
    }
    string adminList() const {
        string o = "{\"ok\":true,\"total\":" + to_string(bank.size()) + ",\"subjects\":" + subjectCountsJson() + ",\"questions\":[";
        for (size_t i = 0; i < bank.size(); i++) {
            const Question& Q = bank[i];
            if (i) o += ",";
            o += "{\"i\":" + to_string(i) + ",\"subject\":" + q(Q.subject) + ",\"text\":" + q(Q.text) + ",\"options\":["
                 + q(Q.opt[0]) + "," + q(Q.opt[1]) + "," + q(Q.opt[2]) + "," + q(Q.opt[3]) + "],\"correct\":" + to_string(Q.ans + 1) + "}";
        }
        return o + "]}";
    }
    // mode "replace" = throw the old questions away, "append" = add new ones (duplicates are skipped)
    string adminImport(const string& text, const string& mode) {
        ImportResult R = parseRows(text);
        int added = 0, dup = 0;
        if (R.good.empty()) {
            string e = "No valid question rows found.";
            if (!R.errors.empty()) e += " " + R.errors[0];
            return errJson(e);
        }
        if (mode == "replace") {
            ofstream bk("questions_backup.txt");             // keep the previous file, just in case
            bk << readWholeFile("questions.txt");
            bank = R.good; added = (int)R.good.size();
        } else {
            for (size_t i = 0; i < R.good.size(); i++) {
                bool exists = false;
                for (size_t k = 0; k < bank.size(); k++)
                    if (bank[k].subject == R.good[i].subject && lower(bank[k].text) == lower(R.good[i].text)) { exists = true; break; }
                if (exists) dup++; else { bank.push_back(R.good[i]); added++; }
            }
        }
        saveQuestions();
        rounds.clear();                                       // running rounds point to old question numbers
        return "{\"ok\":true,\"added\":" + to_string(added) + ",\"duplicates\":" + to_string(dup) + ",\"invalid\":" + to_string(R.invalid)
               + ",\"errors\":" + strArray(R.errors) + ",\"total\":" + to_string(bank.size()) + ",\"subjects\":" + subjectCountsJson() + "}";
    }
    string adminAdd(const string& subject, const string& text, const vector<string>& opts, int correct) {
        string row = flatten(subject, 30) + "|" + flatten(text, 300);
        for (size_t i = 0; i < 4; i++) row += "|" + (i < opts.size() ? flatten(opts[i], 120) : string());
        row += "|" + to_string(correct);
        return adminImport(row, "append");
    }
    string adminDelete(int index) {
        if (index < 0 || index >= (int)bank.size()) return errJson("Question not found.");
        bank.erase(bank.begin() + index);
        saveQuestions();
        rounds.clear();
        return "{\"ok\":true,\"total\":" + to_string(bank.size()) + "}";
    }
    vector<string> subjects() const {
        vector<string> s;
        for (size_t i = 0; i < bank.size(); i++)
            if (bank[i].subject != "Sponsor" && find(s.begin(), s.end(), bank[i].subject) == s.end())
                s.push_back(bank[i].subject);
        return s;
    }

    // ---------- Wrong Answer Notebook (wrong.txt: "player|question text") ----------
    void loadWrongBook() {
        ifstream in("wrong.txt");
        string line;
        while (getline(in, line)) {
            size_t p = line.find('|');
            if (p != string::npos) wrongBook[line.substr(0, p)].push_back(line.substr(p + 1));
        }
    }
    void saveWrongBook() const {
        ofstream out("wrong.txt");
        for (map<string, vector<string> >::const_iterator it = wrongBook.begin(); it != wrongBook.end(); ++it)
            for (size_t i = 0; i < it->second.size(); i++) out << it->first << "|" << it->second[i] << "\n";
    }
    void addWrong(const Player& p, const Question& Q) {
        if (Q.subject == "Sponsor") return;
        vector<string>& v = wrongBook[lower(p.name)];
        if (find(v.begin(), v.end(), Q.text) == v.end()) v.push_back(Q.text);
    }
    void removeWrong(const Player& p, const Question& Q) {
        vector<string>& v = wrongBook[lower(p.name)];
        v.erase(remove(v.begin(), v.end(), Q.text), v.end());
    }
    vector<int> wrongIndexes(const Player& p) {
        vector<int> r;
        vector<string>& v = wrongBook[lower(p.name)];
        for (size_t i = 0; i < v.size(); i++)
            for (size_t k = 0; k < bank.size(); k++)
                if (bank[k].text == v[i]) { r.push_back((int)k); break; }
        return r;
    }

    // ---------- helpers ----------
    // deterministic pick: same seed -> same questions in the same order
    vector<int> pick(int n, const string& subject, unsigned seed) const {
        vector<int> all;
        for (size_t i = 0; i < bank.size(); i++) {
            if (bank[i].subject == "Sponsor") continue;
            if (subject.empty() || bank[i].subject == subject) all.push_back((int)i);
        }
        mt19937 g(seed);
        for (int i = (int)all.size() - 1; i > 0; i--) swap(all[i], all[g() % (i + 1)]);
        if ((int)all.size() > n) all.resize(n);
        return all;
    }
    string newToken() {
        char b[32];
        snprintf(b, sizeof b, "%08x%08x", (unsigned)rng(), (unsigned)rng());
        return b;
    }
    bool isDoubleHour() const { return forceDouble || localHour() == 18; }   // 6 PM - 7 PM

    // ---------- Creator level (Notes Marketplace) ----------
    static string levelOf(int sales) {
        if (sales >= 10) return "Gold";
        if (sales >= 5) return "Silver";
        if (sales >= 1) return "Bronze";
        return "None";
    }
    static int feeOf(const string& level) {      // higher level = lower fee = more income
        if (level == "Gold") return 5;
        if (level == "Silver") return 8;
        return 10;
    }
    // give a seller the income of one sale; returns a message
    string creditSale(Player& seller, Note& n, const string& buyerName) {
        string before = levelOf(market.salesOf(seller.name));
        int fee = n.price * feeOf(before) / 100;
        int net = n.price - fee;
        n.sales++;
        seller.earned += net; seller.totalEarned += net;
        string after = levelOf(market.salesOf(seller.name));
        market.save();
        store.save();
        string m = buyerName + " bought '" + n.title + "' for " + to_string(n.price) + " coins. Platform fee ("
                   + to_string(feeOf(before)) + "%): " + to_string(fee) + ". You earned " + to_string(net) + " coins.";
        if (after != before) m += " Creator level up! You are now a " + after + " creator.";
        return m;
    }

    // ---------- Streak Shield ----------
    // returns messages to show to the player
    vector<string> touchStreak(Player& p) {
        vector<string> msgs;
        long t = localDay() + p.dayOffset;
        if (p.lastDay == t) return msgs;
        long gap = t - p.lastDay;
        if (p.lastDay == 0) p.streak = 1;
        else if (gap == 1) p.streak++;
        else if (gap == 2 && p.shields > 0) {
            p.shields--; p.streak++;
            msgs.push_back("Streak Shield used! You missed a day but your streak is safe.");
        } else {
            if (p.streak > 1) msgs.push_back("You missed some days, so your streak restarted at 1.");
            p.streak = 1;
        }
        if (p.streak % 7 == 0 && p.shields < 1) {
            p.shields++;
            msgs.push_back("7-day streak! You earned a Streak Shield.");
        }
        p.lastDay = t;
        return msgs;
    }

    // ---------- start a round ----------
    string startRound(Player& p, const string& mode, const string& codeParam) {
        vector<int> idx;
        string title, code;
        if (mode == "normal") { idx = pick(5, "", (unsigned)rng()); title = "Normal Quiz"; }
        else if (mode == "friend") {
            unsigned c = (unsigned)toInt(codeParam, 0);
            if (c == 0) c = 100000 + (unsigned)(rng() % 900000);
            else if (c < 100000 || c > 999999) return errJson("Challenge code must be 6 digits.");
            code = to_string(c);
            idx = pick(5, "", c);
            title = "Friend Challenge #" + code;
        } else if (mode == "exam") {
            if (p.examName.empty() || p.examDays <= 0) return errJson("Set your exam first.");
            int n = p.examDays > 30 ? 3 : 5;
            idx = pick(n, p.examSubject, (unsigned)rng());
            title = "Exam Mission: " + p.examSubject;
            if (idx.empty()) return errJson("No questions for subject " + p.examSubject);
        } else if (mode == "retry") {
            idx = wrongIndexes(p);
            if (idx.empty()) return errJson("Your Wrong Answer Notebook is empty. Great job!");
            title = "Retry Mistakes";
        } else if (mode == "sponsor") {
            for (size_t i = 0; i < bank.size(); i++) if (bank[i].subject == "Sponsor") idx.push_back((int)i);
            if (idx.empty()) return errJson("No Sponsor questions found in questions.txt.");
            for (int i = (int)idx.size() - 1; i > 0; i--) swap(idx[i], idx[rng() % (i + 1)]);
            if (idx.size() > 3) idx.resize(3);
            title = "Sponsor Coupon Round";
        } else return errJson("Unknown mode.");

        if (idx.empty()) return errJson("Not enough questions.");
        vector<string> msgs = touchStreak(p);
        store.save();

        Round r;
        r.token = newToken(); r.player = lower(p.name); r.mode = mode; r.title = title;
        r.idx = idx; r.answers.assign(idx.size(), -1);
        rounds[r.token] = r;

        string o = "{\"ok\":true,\"token\":" + q(r.token) + ",\"title\":" + q(title) + ",\"mode\":" + q(mode)
                   + ",\"code\":" + q(code) + ",\"messages\":" + strArray(msgs) + ",\"questions\":[";
        for (size_t i = 0; i < idx.size(); i++) {
            const Question& Q = bank[idx[i]];
            if (i) o += ",";
            o += "{\"subject\":" + q(Q.subject) + ",\"text\":" + q(Q.text) + ",\"options\":["
                 + q(Q.opt[0]) + "," + q(Q.opt[1]) + "," + q(Q.opt[2]) + "," + q(Q.opt[3]) + "]}";
        }
        return o + "]}";
    }

    // ---------- answer one question ----------
    string answer(Player& p, const string& token, int qi, int choice) {
        map<string, Round>::iterator it = rounds.find(token);
        if (it == rounds.end() || it->second.player != lower(p.name)) return errJson("Round not found.");
        Round& r = it->second;
        // choice 0..3 = an option, 4 = time ran out (counts as wrong)
        if (qi < 0 || qi >= (int)r.idx.size() || choice < 0 || choice > 4) return errJson("Bad answer.");
        if (r.answers[qi] != -1) return errJson("Already answered.");
        r.answers[qi] = choice;
        const Question& Q = bank[r.idx[qi]];
        return string("{\"ok\":true,\"correct\":") + (choice == Q.ans ? "true" : "false") + ",\"right\":" + to_string(Q.ans) + "}";
    }

    // ---------- finish round: points + all bonus features ----------
    string finishRound(Player& p, const string& token) {
        map<string, Round>::iterator it = rounds.find(token);
        if (it == rounds.end() || it->second.player != lower(p.name)) return errJson("Round not found.");
        Round r = it->second;
        rounds.erase(it);
        vector<string> msgs;
        int correct = 0, total = (int)r.idx.size();
        for (int i = 0; i < total; i++) {
            const Question& Q = bank[r.idx[i]];
            bool ok = (r.answers[i] == Q.ans);
            if (ok) { correct++; if (r.mode == "retry") removeWrong(p, Q); }
            else addWrong(p, Q);                                   // Wrong Answer Notebook
        }
        saveWrongBook();

        int points = correct * 10;
        if (points > 0 && isDoubleHour()) { points *= 2; msgs.push_back("Double Point Hour! Your points are doubled."); }   // Double Point Hour
        if (points > 0 && points > toppers.best()) { points += 5; msgs.push_back("Beat the Toppers! You beat the best score. +5 bonus."); }   // Beat the Toppers
        if (points > 0) toppers.add(points, p.name);
        p.totalScore += points;
        p.answered += total; p.correctTotal += correct; p.roundsPlayed++;
        if (points > p.bestScore) p.bestScore = points;
        districts.add(p.district, points);                          // District War
        institutions.add(p.institution, points);                    // School/College Championship

        string stamp, coupon;
        bool win = (total >= 3 && correct * 100 >= total * 80);
        if (win && r.mode != "retry" && r.mode != "sponsor" && p.stamps < (int)places.size()) {   // Quiz Passport
            stamp = places[p.stamps];
            p.stamps++;
            msgs.push_back("You won! New passport stamp: " + stamp);
        }
        if (r.mode == "sponsor" && correct >= 2) {                  // Sponsor Coupon Round
            coupon = "DEMO-" + to_string(1000 + rng() % 9000);
            ofstream out("coupons.txt", ios::app);
            out << p.name << "|" << coupon << "\n";
            msgs.push_back("Congratulations! You won a 10% recharge coupon from the sponsor.");
        } else if (r.mode == "sponsor") msgs.push_back("Score at least 2 out of 3 to win the sponsor coupon. Try again!");
        store.save();

        string o = "{\"ok\":true,\"correct\":" + to_string(correct) + ",\"total\":" + to_string(total)
                   + ",\"points\":" + to_string(points) + ",\"messages\":" + strArray(msgs)
                   + ",\"stamp\":" + q(stamp) + ",\"coupon\":" + q(coupon) + ",\"results\":[";
        for (int i = 0; i < total; i++) {
            if (i) o += ",";
            const Question& Q = bank[r.idx[i]];
            o += string("{\"ok\":") + (r.answers[i] == Q.ans ? "true" : "false") + "}";
        }
        return o + "]}";
    }

    // ---------- Notes Marketplace ----------
    string profileJson(Player& p) {
        int sales = market.salesOf(p.name);
        string lv = levelOf(sales);
        int next = sales < 1 ? 1 : sales < 5 ? 5 : sales < 10 ? 10 : 0;
        string o = "{\"ok\":true,\"name\":" + q(p.name) + ",\"district\":" + q(districts.getNames()[p.district])
                   + ",\"institution\":" + q(institutions.getNames()[p.institution])
                   + ",\"streak\":" + to_string(p.streak) + ",\"shields\":" + to_string(p.shields)
                   + ",\"stamps\":" + to_string(p.stamps) + ",\"places\":" + strArray(places)
                   + ",\"totalScore\":" + to_string(p.totalScore)
                   + ",\"coins\":" + to_string(p.coins) + ",\"earned\":" + to_string(p.earned)
                   + ",\"totalEarned\":" + to_string(p.totalEarned)
                   + ",\"pointsAvail\":" + to_string(p.totalScore - p.pointsSpent)
                   + ",\"level\":" + q(lv) + ",\"fee\":" + to_string(feeOf(lv)) + ",\"sales\":" + to_string(sales)
                   + ",\"nextLevelAt\":" + to_string(next)
                   + ",\"exam\":{\"name\":" + q(p.examName) + ",\"days\":" + to_string(p.examDays)
                   + ",\"subject\":" + q(p.examSubject) + ",\"count\":" + to_string(p.examDays > 30 ? 3 : 5) + "}"
                   + ",\"mistakes\":" + to_string(wrongIndexes(p).size())
                   + ",\"answered\":" + to_string(p.answered) + ",\"correctTotal\":" + to_string(p.correctTotal)
                   + ",\"rounds\":" + to_string(p.roundsPlayed) + ",\"bestScore\":" + to_string(p.bestScore)
                   + ",\"playedToday\":" + (p.lastDay == localDay() + p.dayOffset ? "true" : "false")
                   + ",\"doubleNow\":" + (isDoubleHour() ? "true" : "false")
                   + ",\"doubleDemo\":" + (forceDouble ? "true" : "false") + "}";
        return o;
    }

    string marketList(Player& p) {
        string o = "{\"ok\":true,\"notes\":[";
        bool first = true;
        for (size_t i = 0; i < market.all().size(); i++) {
            const Note& n = market.all()[i];
            bool mine = (lower(n.seller) == lower(p.name));
            Player* s = store.find(n.seller);
            string sl = levelOf(market.salesOf(n.seller));
            (void)s;
            if (!first) o += ",";
            first = false;
            o += "{\"id\":" + to_string(n.id) + ",\"title\":" + q(n.title) + ",\"subject\":" + q(n.subject)
                 + ",\"seller\":" + q(n.seller) + ",\"price\":" + to_string(n.price) + ",\"sales\":" + to_string(n.sales)
                 + ",\"sellerLevel\":" + q(sl)
                 + ",\"mine\":" + (mine ? "true" : "false")
                 + ",\"bought\":" + (market.hasBought(p.name, n.id) ? "true" : "false") + "}";
        }
        return o + "]}";
    }

    string buyNote(Player& p, int id) {
        Note* n = market.find(id);
        if (!n) return errJson("Note not found.");
        if (lower(n->seller) == lower(p.name)) return errJson("This is your own note.");
        if (market.hasBought(p.name, id)) return errJson("You already own this note.");
        if (p.coins < n->price)
            return errJson("Not enough coins. You have " + to_string(p.coins) + ", need " + to_string(n->price)
                           + ". Convert quiz points to coins first.");
        p.coins -= n->price;
        market.recordPurchase(p.name, id);
        Player* seller = store.find(n->seller);
        if (seller) creditSale(*seller, *n, p.name);       // the seller earns real coins
        else { n->sales++; market.save(); }                 // starter notes have no real seller
        store.save();
        return "{\"ok\":true,\"message\":" + q("Purchased! You can now open '" + n->title + "'.") + "}";
    }

    string sellNote(Player& p, const string& title, const string& subject, int price, const string& image) {
        if (title.empty() || subject.empty()) return errJson("Title and subject are required.");
        if (price < 1 || price > 500) return errJson("Price must be between 1 and 500 coins.");
        if (image.compare(0, 11, "data:image/") != 0) return errJson("Please choose a picture of your note.");
        if (image.size() > 4 * 1024 * 1024) return errJson("Picture is too big (max 4 MB).");
        int id = market.addNote(p.name, title, subject, price, false);
        market.saveImage(id, image);
        return "{\"ok\":true,\"message\":" + q("Your note is now for sale! Classmates can buy it with coins.") + ",\"id\":" + to_string(id) + "}";
    }

    string viewNote(Player& p, int id) {
        Note* n = market.find(id);
        if (!n) return errJson("Note not found.");
        bool mine = lower(n->seller) == lower(p.name);
        if (!mine && !market.hasBought(p.name, id)) return errJson("Buy this note to open it.");
        string img = market.loadImage(*n);
        if (img.empty()) return errJson("Picture file is missing.");
        return "{\"ok\":true,\"title\":" + q(n->title) + ",\"image\":" + q(img) + "}";
    }

    string convertPoints(Player& p) {
        int avail = p.totalScore - p.pointsSpent;
        int gain = avail / 10;
        if (gain <= 0) return errJson("You need at least 10 quiz points. Play more quizzes!");
        p.coins += gain;
        p.pointsSpent += gain * 10;
        store.save();
        return "{\"ok\":true,\"message\":" + q("Converted " + to_string(gain * 10) + " points into " + to_string(gain) + " coins.") + "}";
    }

    string withdraw(Player& p, int amount) {
        const int MIN_W = 50;
        if (amount < MIN_W) return errJson("Minimum withdrawal is " + to_string(MIN_W) + " coins.");
        if (amount > p.earned) return errJson("You only have " + to_string(p.earned) + " withdrawable coins.");
        p.earned -= amount;
        store.save();
        ofstream out("withdrawals.txt", ios::app);
        out << p.name << " requested " << amount << " BDT\n";
        return "{\"ok\":true,\"message\":" + q("Withdrawal request of " + to_string(amount) + " BDT saved (demo - no real payment).") + "}";
    }

    string demoSale(Player& p) {
        for (size_t i = 0; i < market.all().size(); i++) {
            if (lower(market.all()[i].seller) == lower(p.name)) {
                const char* buyers[] = {"Rahim", "Anika", "Mitu", "Sabbir"};
                string m = creditSale(p, market.all()[i], buyers[market.all()[i].sales % 4]);
                return "{\"ok\":true,\"message\":" + q(m) + "}";
            }
        }
        return errJson("You have no note for sale yet. Sell a note first.");
    }

    string setExam(Player& p, const string& name, int days, const string& subject) {
        vector<string> subs = subjects();
        if (name.empty()) return errJson("Enter your exam name.");
        if (days < 1 || days > 365) return errJson("Days left must be between 1 and 365.");
        if (find(subs.begin(), subs.end(), subject) == subs.end()) return errJson("Unknown subject.");
        p.examName = name; p.examDays = days; p.examSubject = subject;
        store.save();
        return "{\"ok\":true}";
    }
};

// ------------------------------------------------------------------
//  main: web server + API routes
// ------------------------------------------------------------------
int main(int argc, char** argv) {
    int port = (argc > 1) ? toInt(argv[1], 8080) : 8080;
    bool shareWithWifi = (argc > 2 && string(argv[2]) == "share");
    QuizEngine game;
    if (game.bank.empty()) return 1;

    httplib::Server svr;
    svr.set_payload_max_length(8 * 1024 * 1024);
    mutex gate;      // one request at a time keeps the game data safe

    // route(): registers an API function that returns JSON text
    typedef function<string(const httplib::Request&)> Handler;
    auto route = [&](bool isPost, const string& path, Handler fn) {
        auto h = [&gate, fn](const httplib::Request& req, httplib::Response& res) {
            lock_guard<mutex> lock(gate);
            string out;
            try { out = fn(req); } catch (exception& e) { out = errJson(string("Server error: ") + e.what()); }
            res.set_content(out, "application/json; charset=utf-8");
        };
        if (isPost) svr.Post(path, h); else svr.Get(path, h);
    };
    // helper: find the logged-in player from the "name" parameter
    auto who = [&](const httplib::Request& r) -> Player* { return game.store.find(r.get_param_value("name")); };
    const string needLogin = errJson("Please log in first.");

    route(false, "/api/meta", [&](const httplib::Request&) {
        return "{\"ok\":true,\"districts\":" + strArray(game.districts.getNames()) + ",\"institutions\":"
               + strArray(game.institutions.getNames()) + ",\"subjects\":" + strArray(game.subjects())
               + ",\"questions\":" + to_string(game.bank.size()) + "}";
    });
    route(true, "/api/login", [&](const httplib::Request& r) {
        string name = cleanField(r.get_param_value("name"), 24);
        if (name.size() < 2) return errJson("Please enter a name (at least 2 letters).");
        Player* p = game.store.find(name);
        if (!p) {
            if (!r.has_param("district")) return string("{\"ok\":false,\"needSetup\":true}");
            int d = toInt(r.get_param_value("district"), -1), i = toInt(r.get_param_value("institution"), -1);
            if (d < 0 || d >= (int)game.districts.getNames().size() || i < 0 || i >= (int)game.institutions.getNames().size())
                return errJson("Choose your district and school/college.");
            p = game.store.create(name, d, i);
        }
        return game.profileJson(*p);
    });
    route(false, "/api/profile", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        return game.profileJson(*p);
    });
    route(true, "/api/exam/set", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        return game.setExam(*p, cleanField(r.get_param_value("examName"), 30), toInt(r.get_param_value("days")), r.get_param_value("subject"));
    });
    route(true, "/api/quiz/start", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        return game.startRound(*p, r.get_param_value("mode"), r.get_param_value("code"));
    });
    route(true, "/api/quiz/answer", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        return game.answer(*p, r.get_param_value("token"), toInt(r.get_param_value("qi"), -1), toInt(r.get_param_value("choice"), -1));
    });
    route(true, "/api/quiz/finish", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        return game.finishRound(*p, r.get_param_value("token"));
    });
    route(false, "/api/boards", [&](const httplib::Request&) {
        return "{\"ok\":true,\"districts\":" + game.districts.toJson() + ",\"institutions\":" + game.institutions.toJson()
               + ",\"toppers\":" + game.toppers.toJson() + "}";
    });
    route(false, "/api/market/list", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        return game.marketList(*p);
    });
    route(true, "/api/market/buy", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        return game.buyNote(*p, toInt(r.get_param_value("id")));
    });
    route(true, "/api/market/sell", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        return game.sellNote(*p, cleanField(r.get_param_value("title"), 60), cleanField(r.get_param_value("subject"), 30),
                             toInt(r.get_param_value("price")), r.get_param_value("image"));
    });
    route(false, "/api/market/view", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        return game.viewNote(*p, toInt(r.get_param_value("id")));
    });
    route(true, "/api/market/convert", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        return game.convertPoints(*p);
    });
    route(true, "/api/market/withdraw", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        return game.withdraw(*p, toInt(r.get_param_value("amount")));
    });
    // ---- Admin (question bank). Every call needs the admin PIN. ----
    route(true, "/api/admin/login", [&](const httplib::Request& r) {
        string e = game.checkPin(r.get_param_value("pin"));
        if (!e.empty()) return errJson(e);
        return "{\"ok\":true,\"defaultPin\":" + string(game.pinIsDefault() ? "true" : "false") + "}";
    });
    route(true, "/api/admin/list", [&](const httplib::Request& r) {
        string e = game.checkPin(r.get_param_value("pin")); if (!e.empty()) return errJson(e);
        return game.adminList();
    });
    route(true, "/api/admin/import", [&](const httplib::Request& r) {
        string e = game.checkPin(r.get_param_value("pin")); if (!e.empty()) return errJson(e);
        return game.adminImport(r.get_param_value("data"), r.get_param_value("mode"));
    });
    route(true, "/api/admin/add", [&](const httplib::Request& r) {
        string e = game.checkPin(r.get_param_value("pin")); if (!e.empty()) return errJson(e);
        vector<string> o;
        for (int i = 1; i <= 4; i++) o.push_back(r.get_param_value("o" + to_string(i)));
        return game.adminAdd(r.get_param_value("subject"), r.get_param_value("text"), o, toInt(r.get_param_value("correct")));
    });
    route(true, "/api/admin/delete", [&](const httplib::Request& r) {
        string e = game.checkPin(r.get_param_value("pin")); if (!e.empty()) return errJson(e);
        return game.adminDelete(toInt(r.get_param_value("index"), -1));
    });
    // ---- demo tools (to show the teacher) ----
    route(true, "/api/demo/nextday", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        p->dayOffset++; game.store.save();
        return string("{\"ok\":true,\"message\":\"Moved to the next day (demo).\"}");
    });
    route(true, "/api/demo/double", [&](const httplib::Request&) {
        game.forceDouble = !game.forceDouble;
        return string("{\"ok\":true,\"message\":\"Double Point Hour demo is now ") + (game.forceDouble ? "ON" : "OFF") + ".\"}";
    });
    route(true, "/api/demo/sale", [&](const httplib::Request& r) {
        Player* p = who(r); if (!p) return needLogin;
        return game.demoSale(*p);
    });

    // the web page itself (HTML/CSS/JS) is served from the "web" folder
    if (!svr.set_mount_point("/", "./web")) {
        cout << "ERROR: folder 'web' not found. Run the server from the project folder.\n";
        return 1;
    }

    cout << "\n=============================================\n"
         << "  Bangladesh Quiz Arena (C++ web server)\n"
         << "  Open this address in your browser:\n"
         << "      http://localhost:" << port << "\n"
         << (shareWithWifi ? "  Sharing is ON: friends on your Wi-Fi can use http://<your-PC-IP>:" + to_string(port) + "\n"
                           : "  (To let friends join from the same Wi-Fi, start with:  server " + to_string(port) + " share)\n")
         << "  Admin page (add/import questions): open the app, tab 'Me' -> 'Admin'\n"
         << (game.pinIsDefault() ? "  Admin PIN is the default 'admin1234'. Change it: create admin_pin.txt with your own PIN.\n"
                                 : "  Admin PIN: custom PIN is active.\n")
         << "  Press Ctrl+C to stop.\n"
         << "=============================================\n";
    // By default only THIS computer can connect (127.0.0.1). Start with "server 8080 share" to let friends on your Wi-Fi join.
    if (!svr.listen(shareWithWifi ? "0.0.0.0" : "127.0.0.1", port)) {
        cout << "ERROR: could not start on port " << port << ". Is another program using it? Try: server 8081\n";
        return 1;
    }
    return 0;
}
