BANGLADESH QUIZ ARENA - WEB VERSION  (C++ OOP Mini Project)
============================================================

What is what
------------
server.cpp      The MAIN PROGRAM, all in C++ (classes, OOP, file handling, game rules, web API).
httplib.h       Small open-source C++ web-server library (MIT licence, by yhirose). Do not edit.
web/index.html  The screen (HTML/CSS/JavaScript). It only shows buttons and asks the C++ server for everything.
questions.txt   Question bank. Edit it with Notepad. Format:
                Subject|Question|Option1|Option2|Option3|Option4|CorrectOption(1-4)

How to compile and run  (VS Code terminal or Command Prompt, inside THIS folder)
---------------------------------------------------------------------------------
Windows (MinGW):
    g++ -std=c++17 server.cpp -o server -lws2_32 -lwsock32
    server
  (If your g++ says "unrecognized -std=c++17", use -std=c++14 instead.)

Linux / Mac:
    g++ -std=c++17 server.cpp -o server -pthread
    ./server

Then open  http://localhost:8080  in your browser (type it yourself; the program does not open it for you).
Another port:  server 8081   ->  http://localhost:8081
By default only your own computer can connect. To let friends on the same Wi-Fi play against you:
    server 8080 share        then they open  http://<your-PC-IP>:8080
(District War, Toppers and the Marketplace are shared between all players).
Stop the server with Ctrl+C.

If Windows Defender blocks server.exe ("Suspicious behavior"): it is a false alarm for a freshly compiled,
unsigned program that listens on a network port. Use Windows Security > Virus & threat protection > Protection
history > Actions > Allow on device, or add this folder under Manage settings > Exclusions.

IMPORTANT: always start the server from inside this folder, so it can find "web" and "questions.txt".

If compile fails on Windows
---------------------------
- Run  g++ -v  and look at "Thread model". It must say "posix". If it says "win32", your MinGW is too old
  for std::thread. Install MSYS2 or WinLibs (MinGW-w64, posix threads) and try again.
- "undefined reference to WSA..." means -lws2_32 -lwsock32 was forgotten.

Admin page: add or import questions without touching the code
-------------------------------------------------------------
Open the app -> tab "Me" -> button "Admin: manage questions". Default PIN: admin1234
(to set your own PIN, create a file admin_pin.txt next to the server containing the PIN, then restart;
 on a public link, ALWAYS change it).
You can:
  - import an Excel file (.xlsx), a Word file (.docx, questions in a table), a CSV or a text file,
    or paste rows copied from Excel/Word. The rows are shown in a box so you can check them first.
  - choose "Add to existing" or "Replace ALL" (the old file is saved as questions_backup.txt)
  - add one question with a form, delete questions, search/filter, download everything as CSV
Columns (in this order):  Subject | Question | Option 1 | Option 2 | Option 3 | Option 4 | Correct (1-4 or A-D)
The first row may be a header. Subject "Sponsor" = questions for the Sponsor Coupon Round.
Rows with problems are skipped and listed with their line numbers. Changes work immediately (no restart).
Old .xls/.doc files are not supported: in Excel/Word use  Save As -> .xlsx / .docx / CSV.

Files the game creates (saved data, safe to delete to start fresh)
-----------------------------------------------------------------
players.txt  market.txt  purchases.txt  wrong.txt  toppers.txt  districts.txt  institutions.txt
coupons.txt  withdrawals.txt  questions_backup.txt  uploads/ (photos of notes)
(questions.txt is your question bank: do not delete it.)

The 10 features + Notes Marketplace (all calculated in C++)
-----------------------------------------------------------
1  District vs District War      5 districts, points of every player are added to their district
2  School/College Championship   3 institutions
3  Exam Countdown Mission        exam name + days left + subject -> daily mission (3 or 5 MCQs)
4  Streak Shield                 7 days in a row = 1 shield; a shield saves the streak when you miss a day
5  Quiz Passport                 win a round (80%+) = 1 stamp (Dhaka, Cox's Bazar, Padma Bridge, Sylhet, Sundarbans)
6  Beat the Toppers              best 3 rounds shown; beating the best score gives +5 bonus
7  Double Point Hour             6 PM - 7 PM all points x2
8  Friend Challenge Code         same 6-digit code = same questions in the same order
9  Wrong Answer Notebook         every mistake is saved; "Retry Mistakes" plays them again
10 Sponsor Coupon Round          3 sponsor questions, 2 right = coupon code (saved in coupons.txt)
11 Notes Marketplace (UNIQUE)    sell photos of your handwritten notes for coins; other students buy and open them;
                                 seller income = price - platform fee; Creator level Bronze/Silver/Gold lowers the fee
                                 (10% / 8% / 5%); points convert to coins (10 points = 1 coin); withdraw request (demo).

Demo tools (Me tab) let you show the teacher streaks, double points and a sale without waiting.
Real payments are NOT implemented: withdrawals are only written to withdrawals.txt (demo).
There is no password: a player is identified by name only (fine for a class project).

C++ classes (for the UML diagram)
---------------------------------
Question, Player, PlayerStore, ScoreBoard, TopScores, Note, Marketplace, Round, QuizEngine
QuizEngine has: PlayerStore, 2 x ScoreBoard, TopScores, Marketplace, many Question objects, many Round objects.
QuizEngine also contains the admin functions (import/add/delete questions, PIN check).
Marketplace has many Note objects.  PlayerStore has many Player objects.
