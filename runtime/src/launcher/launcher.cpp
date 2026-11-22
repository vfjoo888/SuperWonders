// SuperWonders Launcher (launcher.h): state, the setup job, and the screen.
#include "launcher/launcher.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <nx/gamelib.h>
#include <nx/runtime.h>

#include "discord_rpc.h"
#include "imgui.h"
#include "launcher/game_version.h"
#include "launcher/test_pad.h"
#include "launcher/updates.h"
#include "nvn/nvn_vk.h"
#include "overlay/option_rows.h"
#include "overlay/overlay.h"
#include "overlay/overlay_internal.h"
#include "overlay/overlay_ui.h"
#include "platform.h"
#include "user_profiles.h"
#include "profile_image.h"

namespace fs = std::filesystem;

namespace nx::launcher {

namespace ui = overlay::ui;
namespace rows = overlay::rows;

namespace {

fs::path u8(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }
std::string str(const fs::path& p) {
    auto s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}
bool file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
#if !defined(_WIN32)
fs::path self_exe_path() {
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : p;
}
#endif
std::string read_line(const fs::path& p, const char* key) {  // "key value" lines (build-id.txt)
    std::ifstream f(p);
    std::string line, k = std::string(key) + " ";
    while (std::getline(f, line))
        if (line.rfind(k, 0) == 0) return line.substr(k.size());
    return {};
}

// ---- launcher.ini: what the player picked (in the data root) ----
struct Config {
    std::string exefs, romfs;
    int romfs_copy = 1;  // 1 copy the RomFS into the data root (the default, as Savage Wind does), 0 use it in place
};

fs::path config_path() { return u8(plat::data_root()) / "launcher.ini"; }

Config load_config() {
    Config c;
    std::ifstream f(config_path());
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "exefs") c.exefs = v;
        else if (k == "romfs") c.romfs = v;
        else if (k == "romfs_copy") c.romfs_copy = v != "0";
    }
    return c;
}

void save_config(const Config& c) {
    plat::make_dirs(plat::data_root());
    std::ofstream f(config_path(), std::ios::trunc);
    f << "exefs=" << c.exefs << "\nromfs=" << c.romfs << "\nromfs_copy=" << c.romfs_copy << "\n";
}

// ---- the game-build kit: tools/setup_game.py and a Python to run it ----
struct Kit {
    std::string python, script;  // empty: none
    std::string id;              // game-build/kit-id.txt: what the release's game code is built with
};

Kit find_kit() {
    Kit k;
    const fs::path kit = u8(program_dir()) / "game-build";
#if defined(_WIN32)
    const fs::path py = kit / "python" / "python.exe";
#else
    const fs::path py = file(kit / "python" / "bin" / "python3") ? kit / "python" / "bin" / "python3" : fs::path("python3");
#endif
    if (file(kit / "setup_game.py")) {
        k.script = str(kit / "setup_game.py");
        k.python = py.is_absolute() ? str(py) : "python3";
        std::ifstream f(kit / "kit-id.txt");
        std::getline(f, k.id);
    } else {
        // Fallback for local CMake builds (e.g. build-git-win): locate checkout tools/ and portable python.
        fs::path cur = u8(program_dir());
        for (int depth = 0; depth < 4 && !cur.empty(); ++depth) {
            for (const fs::path& cand : {cur / "tools" / "setup_game.py",
                                         cur / "SuperWonders-git" / "tools" / "setup_game.py"}) {
                if (file(cand)) {
                    k.script = str(cand);
                    break;
                }
            }
#if defined(_WIN32)
            for (const fs::path& py_cand : {cur / "SuperWonders-builder-portable" / "tools" / "portable" / "python" / "python.exe",
                                            cur / "tools" / "portable" / "python" / "python.exe"}) {
                if (file(py_cand)) {
                    k.python = str(py_cand);
                    break;
                }
            }
#endif
            if (!k.script.empty()) break;
            fs::path parent = cur.parent_path();
            if (parent == cur) break;
            cur = parent;
        }
    }
    // a developer's build: NX_SETUP_SCRIPT=<checkout>/tools/setup_game.py, NX_SETUP_PYTHON=<a Python with the deps>
    if (const char* s = std::getenv("NX_SETUP_SCRIPT"); s && *s) k.script = s;
    if (const char* p = std::getenv("NX_SETUP_PYTHON"); p && *p) k.python = p;
    if (!k.script.empty() && k.python.empty()) k.python =
#if defined(_WIN32)
        "python";
#else
        "python3";
#endif
    return k;
}

// ---- is the game set up? ----
enum class Ready { Yes, NotSetUp, Rebuild };
struct Status {
    Ready ready = Ready::NotSetUp;
    bool kit_outdated = false;
    std::string installed_version;
    std::string why;
};

Status check_status(const Kit& kit) {
    const fs::path d = u8(plat::data_root());
    const OutputInfo out = inspect_output_dir(d, NX_GAMELIB_FILE, game_lib_path());
    Status s;
    s.installed_version = out.installed_version;
    if (!out.ready) {
        s.why = out.missing_reason;
        return s;
    }
    const std::string built = read_line(d / "game" / "build-id.txt", "kit");
    const std::string local_lib = str(d / "game" / NX_GAMELIB_FILE);
    if (!kit.id.empty() && !built.empty() && built != kit.id && game_lib_path() == local_lib) {
        std::string abi_why;
        if (!probe_game_lib(local_lib, abi_why)) {
            s.ready = Ready::Rebuild;
            s.why = abi_why.empty() ? "This version of SuperWonders requires rebuilding the game code." : abi_why;
            return s;
        }
        s.kit_outdated = true;
    }
    s.ready = Ready::Yes;
    return s;
}

// ---- the setup job: tools/setup_game.py in a process of its own, its progress lines read here ----
class Job {
public:
    std::atomic<bool> running{false};
    std::atomic<float> progress{0};
    std::atomic<int> result{-1};  // exit code of the last run, -1 none yet
    std::mutex lock;
    std::string what;                // the current step
    std::vector<std::string> tail;   // the last lines (shown when it fails)
    uint64_t started_ns = 0;

    void start(const std::vector<std::string>& args) {
        if (running) return;
        if (worker.joinable()) worker.join();
        {
            std::lock_guard lk(lock);
            what = "Starting";
            tail.clear();
        }
        progress = 0, result = -1, running = true, cancelled = false;
        started_ns = plat::monotonic_ns();
        worker = std::thread([this, args] { run(args); });
    }
    void cancel() {
        cancelled = true;
#if defined(_WIN32)
        if (job) TerminateJobObject(job, 1);
#else
        if (pid > 0) kill(-pid, SIGTERM);
#endif
    }
    bool was_cancelled() const { return cancelled; }
    ~Job() {
        if (running) cancel();
        if (worker.joinable()) worker.join();
    }

private:
    std::thread worker;
    std::atomic<bool> cancelled{false};
#if defined(_WIN32)
    HANDLE job = nullptr;
#else
    pid_t pid = -1;
#endif

    void line(std::string l) {
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
        if (l.empty()) return;
        float f;
        char text[512] = "";
        if (std::sscanf(l.c_str(), "progress %f %511[^\n]", &f, text) >= 1) {
            progress = std::clamp(f, 0.0f, 1.0f);
            std::lock_guard lk(lock);
            if (*text) what = text;
            return;
        }
        log("launcher: setup: %s", l.c_str());
        std::lock_guard lk(lock);
        tail.push_back(l);
        if (tail.size() > 12) tail.erase(tail.begin());
    }

    void run(std::vector<std::string> args) {
        int code = 1;
#if defined(_WIN32)
        auto wide = [](const std::string& s) {
            int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            std::wstring w(size_t(std::max(1, n)) - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
            return w;
        };
        std::wstring cmd;
        for (const std::string& a : args) {
            std::wstring w = wide(a);
            cmd += (cmd.empty() ? L"\"" : L" \"") + w + L"\"";  // (no argument here ends in a backslash or has quotes)
        }
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        CreatePipe(&rd, &wr, &sa, 0);
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = wr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        // A job object: cancelling ends the whole tree (the compiler and translator processes too).
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                           nullptr, &si, &pi)) {
            AssignProcessToJobObject(job, pi.hProcess);
            ResumeThread(pi.hThread);
            CloseHandle(wr);
            wr = nullptr;
            std::string buf;
            char chunk[4096];
            DWORD n = 0;
            while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n) {
                buf.append(chunk, n);
                for (size_t nl; (nl = buf.find('\n')) != std::string::npos; buf.erase(0, nl + 1)) line(buf.substr(0, nl));
            }
            line(buf);
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD ec = 1;
            GetExitCodeProcess(pi.hProcess, &ec);
            code = int(ec);
            CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
        } else {
            line("error: couldn't start " + args[0] + " (error " + std::to_string(GetLastError()) + ")");
        }
        if (wr) CloseHandle(wr);
        CloseHandle(rd);
        CloseHandle(job);
        job = nullptr;
#else
        int fds[2];
        if (pipe(fds) == 0) {
            pid = fork();
            if (pid == 0) {
                setpgid(0, 0);  // its own process group: cancelling ends the whole tree
                dup2(fds[1], 1), dup2(fds[1], 2);
                close(fds[0]), close(fds[1]);
                std::vector<char*> argv;
                for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
                argv.push_back(nullptr);
                execvp(argv[0], argv.data());
                std::fprintf(stderr, "error: couldn't start %s\n", argv[0]);
                _exit(127);
            }
            close(fds[1]);
            std::string buf;
            char chunk[4096];
            for (ssize_t n; (n = read(fds[0], chunk, sizeof chunk)) > 0;) {
                buf.append(chunk, size_t(n));
                for (size_t nl; (nl = buf.find('\n')) != std::string::npos; buf.erase(0, nl + 1)) line(buf.substr(0, nl));
            }
            line(buf);
            close(fds[0]);
            int st = 0;
            waitpid(pid, &st, 0);
            code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
            pid = -1;
        }
#endif
        result = cancelled ? 2 : code;
        running = false;
    }
};

// ---- mods: <data root>/mods/<name>/ (hle_fs.cpp mod_roots: name order, a leading '-' turns one off) ----
struct Mod {
    std::string name;  // without the '-'
    bool on;
};

std::vector<Mod> list_mods() {
    std::vector<Mod> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(u8(plat::data_root()) / "mods", ec)) {
        if (!e.is_directory()) continue;
        std::string n = str(e.path().filename());
        bool on = n.rfind('-', 0) != 0;
        out.push_back({on ? n : n.substr(1), on});
    }
    std::sort(out.begin(), out.end(), [](const Mod& a, const Mod& b) { return a.name < b.name; });
    return out;
}

bool set_mod(const Mod& m, bool on) {
    const fs::path dir = u8(plat::data_root()) / "mods";
    std::error_code ec;
    fs::rename(dir / u8(m.on ? m.name : "-" + m.name), dir / u8(on ? m.name : "-" + m.name), ec);
    return !ec;
}

// ---- the launcher's state (the screen runs on the presenting thread, run() on the game's) ----
enum Page { PagePlay, PageFiles, PageSettings, PageMods, PageUpdates, PageQuit, kPages };
const char* const kPageNames[kPages] = {"Play", "Game files", "Settings", "Mods", "Updates", "Quit"};

enum LauncherText {
    LtPlay, LtGameFiles, LtSettings, LtMods, LtUpdates, LtQuit, LtSuperWonders, LtAbout, LtYourGameFiles,
    LtOutputFolder, LtExeFS, LtRomFSFolder, LtGameVersion, LtChoose, LtUseInPlace, LtCopyIt, LtSetUp, LtSetUpGameFiles,
    LtOpenDataFolder, LtOpenSettings, LtFolder, LtOpenModsFolder, LtRefresh, LtQuitSuperWonders, LtFilesReady,
    LtNeedsRebuild, LtFilesNotSetUp, LtSelect, LtBack, LtPages, LtPlayHint, LtStart, LtSetUpFirst, LtVersion,
    LtChooseOutput, LtChooseExeFS, LtChooseRomFS, LtUseFolder, LtResetDefault, LtUpFolder, LtHomeFolder, LtDrives,
    LtFoldersHere, LtOtherWays, LtSystemPicker, LtCancel, LtSetUpAgain, LtTextCount
};

// These short labels follow the language selected in F1 > System; the detailed tool output stays in its original language.
const char* const kLauncherText[10][LtTextCount] = {
    {"Play", "Game files", "Settings", "Mods", "Updates", "Quit", "SuperWonders", "About", "Your game files",
     "Output folder", "ExeFS folder", "RomFS folder", "Game version", "Choose...", "Use it in place", "Copy it",
     "Set up", "Set up game files", "Open output folder", "Open settings", "Folder", "Open the mods folder", "Refresh",
     "Quit SuperWonders", "Game files ready", "Game code needs building again", "Game files not set up", "Select", "Back",
     "Pages", "Play", "Start", "Set up first", "Version", "Choose your output folder", "Choose your ExeFS folder",
     "Choose your RomFS folder", "Use this folder", "Use default folder", "Up one folder", "Home folder", "Drives",
     "Folders here", "Other ways", "The system's folder picker", "Cancel", "Set up again"},
    {"Gioca", "File di gioco", "Impostazioni", "Mod", "Aggiornamenti", "Esci", "SuperWonders", "Informazioni",
     "File del gioco", "Cartella di output", "Cartella ExeFS", "Cartella RomFS", "Versione gioco", "Scegli...",
     "Usa nella posizione", "Copia", "Configurazione", "Configura i file di gioco", "Apri cartella di output",
     "Apri impostazioni", "Cartella", "Apri cartella mod", "Aggiorna", "Esci da SuperWonders", "File di gioco pronti",
     "Ricompila il codice di gioco", "File di gioco non configurati", "Seleziona", "Indietro", "Pagine", "Gioca",
     "Avvia", "Configura prima", "Versione", "Scegli cartella di output", "Scegli la cartella ExeFS",
     "Scegli la cartella RomFS", "Usa questa cartella", "Usa cartella predefinita", "Cartella superiore",
     "Cartella home", "Unità", "Cartelle qui", "Altri metodi", "Selettore cartelle di sistema", "Annulla",
     "Configura di nuovo"},
    {"Spielen", "Spieldateien", "Einstellungen", "Mods", "Updates", "Beenden", "SuperWonders", "Info",
     "Deine Spieldateien", "Ausgabeordner", "ExeFS-Ordner", "RomFS-Ordner", "Spielversion", "Auswählen...",
     "Am Ort verwenden", "Kopieren", "Einrichten", "Spieldateien einrichten", "Ausgabeordner öffnen",
     "Einstellungen öffnen", "Ordner", "Mods-Ordner öffnen", "Aktualisieren", "SuperWonders beenden",
     "Spieldateien bereit", "Spielcode muss neu erstellt werden", "Spieldateien nicht eingerichtet", "Auswählen",
     "Zurück", "Seiten", "Spielen", "Starten", "Zuerst einrichten", "Version", "Ausgabeordner auswählen",
     "ExeFS-Ordner auswählen", "RomFS-Ordner auswählen", "Diesen Ordner verwenden", "Standardordner verwenden",
     "Übergeordneter Ordner", "Benutzerordner", "Laufwerke", "Ordner hier", "Weitere Möglichkeiten",
     "System-Dateiauswahl", "Abbrechen", "Erneut einrichten"},
    {"Jouer", "Fichiers du jeu", "Paramètres", "Mods", "Mises à jour", "Quitter", "SuperWonders", "À propos",
     "Fichiers du jeu", "Dossier de sortie", "Dossier ExeFS", "Dossier RomFS", "Version du jeu", "Choisir...",
     "Utiliser sur place", "Copier", "Configurer", "Configurer les fichiers du jeu", "Ouvrir le dossier de sortie",
     "Ouvrir les paramètres", "Dossier", "Ouvrir le dossier des mods", "Actualiser", "Quitter SuperWonders",
     "Fichiers du jeu prêts", "Le code du jeu doit être recompilé", "Fichiers du jeu non configurés", "Sélectionner",
     "Retour", "Pages", "Jouer", "Démarrer", "Configurer d'abord", "Version", "Choisir le dossier de sortie",
     "Choisir le dossier ExeFS", "Choisir le dossier RomFS", "Utiliser ce dossier", "Dossier par défaut",
     "Dossier parent", "Dossier personnel", "Lecteurs", "Dossiers ici", "Autres méthodes",
     "Sélecteur de dossiers système", "Annuler", "Reconfigurer"},
    {"Jugar", "Archivos del juego", "Ajustes", "Mods", "Actualizaciones", "Salir", "SuperWonders", "Acerca de",
     "Archivos del juego", "Carpeta de salida", "Carpeta ExeFS", "Carpeta RomFS", "Versión del juego", "Elegir...",
     "Usar en su ubicación", "Copiar", "Configurar", "Configurar archivos del juego", "Abrir carpeta de salida",
     "Abrir ajustes", "Carpeta", "Abrir carpeta de mods", "Actualizar", "Salir de SuperWonders",
     "Archivos del juego listos", "Hay que recompilar el código del juego", "Archivos del juego sin configurar",
     "Seleccionar", "Atrás", "Páginas", "Jugar", "Iniciar", "Configura primero", "Versión", "Elegir carpeta de salida",
     "Elegir carpeta ExeFS", "Elegir carpeta RomFS", "Usar esta carpeta", "Carpeta predeterminada", "Carpeta superior",
     "Carpeta de inicio", "Unidades", "Carpetas aquí", "Otras opciones", "Selector de carpetas del sistema",
     "Cancelar", "Configurar de nuevo"},
    {"플레이", "게임 파일", "설정", "모드", "업데이트", "종료", "SuperWonders", "정보", "게임 파일", "출력 폴더",
     "ExeFS 폴더", "RomFS 폴더", "게임 버전", "선택...", "현재 위치에서 사용", "복사", "설정", "게임 파일 설정",
     "출력 폴더 열기", "설정 열기", "폴더", "모드 폴더 열기", "새로고침", "SuperWonders 종료", "게임 파일 준비 완료",
     "게임 코드를 다시 빌드해야 합니다", "게임 파일이 설정되지 않음", "선택", "뒤로", "페이지", "플레이", "시작",
     "먼저 설정", "버전", "출력 폴더 선택", "ExeFS 폴더 선택", "RomFS 폴더 선택", "이 폴더 사용", "기본 폴더 사용",
     "상위 폴더", "홈 폴더", "드라이브", "현재 폴더", "다른 방법", "시스템 폴더 선택기", "취소", "다시 설정"},
    {"プレイ", "ゲームファイル", "設定", "MOD", "更新", "終了", "SuperWonders", "概要", "ゲームファイル",
     "出力フォルダー", "ExeFS フォルダー", "RomFS フォルダー", "ゲームバージョン", "選択...", "この場所で使う", "コピー",
     "セットアップ", "ゲームファイルを設定", "出力フォルダーを開く", "設定を開く", "フォルダー", "MODフォルダーを開く",
     "更新", "SuperWondersを終了", "ゲームファイルの準備完了", "ゲームコードを再ビルドしてください", "ゲームファイル未設定",
     "選択", "戻る", "ページ", "プレイ", "開始", "先に設定", "バージョン", "出力フォルダーを選択", "ExeFSフォルダーを選択",
     "RomFSフォルダーを選択", "このフォルダーを使う", "標準フォルダーを使う", "上のフォルダー", "ホームフォルダー", "ドライブ",
     "この場所のフォルダー", "その他の方法", "システムのフォルダー選択", "キャンセル", "再設定"},
    {"تشغيل", "ملفات اللعبة", "الإعدادات", "التعديلات", "التحديثات", "خروج", "SuperWonders", "حول",
     "ملفات اللعبة", "مجلد الإخراج", "مجلد ExeFS", "مجلد RomFS", "إصدار اللعبة", "اختيار...", "استخدم في مكانه", "نسخ",
     "إعداد", "إعداد ملفات اللعبة", "فتح مجلد الإخراج", "فتح الإعدادات", "مجلد", "فتح مجلد التعديلات", "تحديث",
     "إنهاء SuperWonders", "ملفات اللعبة جاهزة", "يجب إعادة بناء كود اللعبة", "لم يتم إعداد ملفات اللعبة", "اختيار",
     "رجوع", "الصفحات", "تشغيل", "ابدأ", "أعد الإعداد أولاً", "الإصدار", "اختر مجلد الإخراج", "اختر مجلد ExeFS",
     "اختر مجلد RomFS", "استخدم هذا المجلد", "استخدام المجلد الافتراضي", "المجلد الأعلى", "المجلد الرئيسي",
     "محركات الأقراص", "المجلدات هنا", "طرق أخرى", "منتقي مجلدات النظام", "إلغاء", "إعادة الإعداد"},
    {"Играть", "Файлы игры", "Настройки", "Моды", "Обновления", "Выход", "SuperWonders", "О программе",
     "Файлы игры", "Папка вывода", "Папка ExeFS", "Папка RomFS", "Версия игры", "Выбрать...",
     "Использовать на месте", "Копировать", "Настройка", "Настроить файлы игры", "Открыть папку вывода",
     "Открыть настройки", "Папка", "Открыть папку модов", "Обновить", "Выйти из SuperWonders",
     "Файлы игры готовы", "Нужно пересобрать код игры", "Файлы игры не настроены", "Выбрать", "Назад", "Страницы",
     "Играть", "Запустить", "Сначала настройте", "Версия", "Выберите папку вывода", "Выберите папку ExeFS",
     "Выберите папку RomFS", "Использовать эту папку", "Папка по умолчанию", "На уровень выше", "Домашняя папка",
     "Диски", "Папки здесь", "Другие способы", "Системный выбор папки", "Отмена", "Настроить снова"},
    {"Oyna", "Oyun dosyaları", "Ayarlar", "Modlar", "Güncellemeler", "Çıkış", "SuperWonders", "Hakkında",
     "Oyun dosyalarınız", "Çıktı klasörü", "ExeFS klasörü", "RomFS klasörü", "Oyun sürümü", "Seç...", "Yerinde kullan",
     "Kopyala", "Kurulum", "Oyun dosyalarını kur", "Çıktı klasörünü aç", "Ayarları aç", "Klasör",
     "Mod klasörünü aç", "Yenile", "SuperWonders'tan çık", "Oyun dosyaları hazır", "Oyun kodu yeniden derlenmeli",
     "Oyun dosyaları kurulmadı", "Seç", "Geri", "Sayfalar", "Oyna", "Başlat", "Önce kur", "Sürüm",
     "Çıktı klasörünü seç", "ExeFS klasörünü seç", "RomFS klasörünü seç", "Bu klasörü kullan", "Varsayılan klasör",
     "Üst klasör", "Ana klasör", "Sürücüler", "Buradaki klasörler", "Diğer yollar", "Sistem klasör seçicisi",
     "İptal", "Yeniden kur"},
};

const char* lt(LauncherText text) {
    const int language = overlay::menu_language();
    if (language >= 10) {
        // Translate the launcher's main navigation for the newly added locales;
        // less common setup/help strings fall back to English for now.
        static constexpr const char* navigation[3][6] = {
            {"Spelen", "Spelbestanden", "Instellingen", "Mods", "Updates", "Afsluiten"},
            {"开始游戏", "游戏文件", "设置", "模组", "更新", "退出"},
            {"開始遊戲", "遊戲檔案", "設定", "模組", "更新", "離開"},
        };
        if (text >= LtPlay && text <= LtQuit) return navigation[language - 10][text - LtPlay];
    }
    // The existing launcher help and setup text has no Dutch or Chinese translation yet.
    const int safe_language = std::min(language, 9);
    return kLauncherText[safe_language][text];
}

struct State {
    Kit kit;
    Config config;
    Status status;
    std::vector<Mod> mods;
    std::atomic<int> page{PagePlay};
    std::atomic<bool> play{false}, quit{false};
    bool config_mode = false;
    Job job;
    std::mutex pick_lock;
    std::string picked_exefs, picked_romfs, picked_output;  // from the folder picker's thread
    ExefsInfo exefs_info;
    RomfsInfo romfs_info;
    PairInfo pair_info;
    uint64_t shown_ns = 0;
    std::string note;  // a line under the status (an error of the folder picker, a finished setup...)
    bool demo_progress = false;  // NX_LAUNCHER_TOUR's last step: the setup card, not running anything
    bool open_lang_warning = false;  // 1.2.1-only game language selected on another game version
    // The controller (Savage Wind's launcher: the page list and the page are two places the cursor can be; A or right
    // goes into the page, B back to the list, L / R change the page, + plays). One request at a time, next frame.
    bool focus_list = true;   // the cursor to the selected page's name
    int follow_hold = 0;      // frames the page doesn't follow the cursor (L / R moved it; the cursor catches up)
    bool in_list = true;      // where it is now
    uint64_t pad_before = 0;  // the pad's buttons last frame (edges)
    // The folder browser (a list a controller can walk; the system's picker needs a mouse): 0 closed, 1 for the
    // ExeFS, 2 for the RomFS, 3 for the Output folder. "" as the folder: the drives (Windows) or / (elsewhere).
    int browse = 0;
    std::string browse_dir;
    std::vector<std::string> browse_list;  // its subfolders, read when it changes
    std::string browse_listed;
    std::string browse_inspected;
    ExefsInfo browse_exefs_info;
    RomfsInfo browse_romfs_info;
    OutputInfo browse_output_info;
};
State* g = nullptr;

void refresh_input_versions() {
    g->exefs_info = inspect_exefs(u8(g->config.exefs));
    g->romfs_info = inspect_romfs(u8(g->config.romfs));
    g->pair_info = evaluate_pair(g->exefs_info, g->romfs_info);
}

void refresh_status() {
    g->status = check_status(g->kit);
    g->mods = list_mods();
    refresh_input_versions();
    const std::string& active_ver =
        !g->status.installed_version.empty() ? g->status.installed_version
                                             : (g->exefs_info.compatible ? g->exefs_info.version : std::string{});
    overlay::set_game_version(active_ver);
}

void apply_output_folder(const std::string& dir) {
    if (!plat::set_data_root(dir)) {
        g->note = "Couldn't use output folder: " + dir;
        return;
    }
    plat::reload_pc_settings();
    const Config next = load_config();
    if (!next.exefs.empty() || !next.romfs.empty()) g->config = next;
    if (g->job.result > 0) g->job.result = -1;
    refresh_status();
    if (g->status.ready == Ready::Yes) {
        g->note = "Output folder ready" +
                  (g->status.installed_version.empty() ? std::string() : " (" + g->status.installed_version + ")") +
                  " - no recompilation needed.";
    } else {
        g->note = "Output folder set: " + plat::data_root();
    }
    log("launcher: output folder %s (ready=%s, version=%s)", plat::data_root().c_str(),
        g->status.ready == Ready::Yes ? "yes" : "no",
        g->status.installed_version.empty() ? "unknown" : g->status.installed_version.c_str());
}

// A folder for a row's value, fitted to the room the row leaves beside its label (in pixels, at the current font and
// scale): the whole path when it fits, else its last parts after "...", else the end of its last part. Measured, so
// a narrow window, a large display scale or a long path can't make the value run over the label.
std::string fit_path(const std::string& path, const char* label) {
    std::string s = path;
    for (char& c : s) c = c == '\\' ? '/' : c;
    std::string up = label;
    for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
    const float room = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(up.c_str()).x -
                       4 * ImGui::GetStyle().FramePadding.x - 3 * ImGui::GetFontSize();  // (padding, the gap)
    auto fits = [&](const std::string& t) { return ImGui::CalcTextSize(t.c_str()).x <= room; };
    if (fits(s)) return s;
    for (size_t at = s.find('/'); at != std::string::npos; at = s.find('/', at + 1)) {
        const std::string t = "..." + s.substr(at);
        if (fits(t)) return t;
    }
    for (size_t cut = 1; cut < s.size(); ++cut) {  // not even the last part fits: its end
        const std::string t = "..." + s.substr(cut);
        if (fits(t)) return t;
    }
    return "...";
}

bool folder_has(const std::string& dir, const char* name) {
    std::error_code ec;
    return !dir.empty() && fs::exists(u8(dir) / name, ec);
}

void start_setup() {
    if (g->kit.script.empty()) {
        g->note = "This build of SuperWonders has no game-build folder, so it can't set the game up.";
        return;
    }
    save_config(g->config);
    g->job.start({g->kit.python, "-u", g->kit.script, "setup", "--exefs", g->config.exefs, "--romfs", g->config.romfs,
                  "--data-root", plat::data_root(), "--romfs-mode", g->config.romfs_copy ? "copy" : "link"});
    log("launcher: setup started (ExeFS %s, RomFS %s, %s)", g->config.exefs.c_str(), g->config.romfs.c_str(),
        g->config.romfs_copy ? "copy" : "link");
}

// ---- drawing ----
float g_k = 1;

void backdrop(ImDrawList* dl, ImVec2 ds) {
    const ui::Theme& th = ui::theme();
    dl->AddRectFilledMultiColor({0, 0}, ds, th.back_top, th.back_top, th.back_mid, th.back_mid);
    // Subtle ambient accent glow in the upper right, kept stationary for a clean minimalist look.
    const float gx = ds.x * 0.78f, gy = ds.y * 0.16f, r = ds.y * 0.65f;
    const ImU32 in = (th.accent & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 16), out = in & ~IM_COL32_A_MASK;
    constexpr int kSeg = 48;
    dl->PrimReserve(kSeg * 3, kSeg + 1);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx({gx, gy}, uv, in);
    for (int i = 0; i < kSeg; ++i) {
        const float a = float(i) / kSeg * 6.2831853f;
        dl->PrimWriteVtx({gx + std::cos(a) * r, gy + std::sin(a) * r}, uv, out);
    }
    for (int i = 0; i < kSeg; ++i) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx(ImDrawIdx(base + 1 + i));
        dl->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % kSeg));
    }
}

// The setup's progress: a minimalist card matching the in-game settings aesthetic.
void progress_card(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    const ui::Theme& th = ui::theme();
    const float k = g_k;
    static float shown = 0;
    const float frac = g->job.progress;
    shown = g->job.running ? ui::approach(shown, frac, 5.0f) : frac;
    std::string what;
    {
        std::lock_guard lk(g->job.lock);
        what = g->job.what;
    }
    ImFont* tf = ui::title();
    ImFont* bf = ui::body();
    const float tfs = 42 * k, sfs = 25 * k;
    dl->AddRectFilled(p0, p1, th.card, 14 * k);
    dl->AddRect(p0, p1, ui::fade(IM_COL32(255, 255, 255, 22)), 14 * k, 0, std::max(1.0f, k));
    const char* title = "Setting up your game";
    const ImVec2 ts = tf->CalcTextSizeA(tfs, FLT_MAX, 0, title);
    dl->AddText(tf, tfs, {(p0.x + p1.x - ts.x) * 0.5f, p0.y + 40 * k}, th.text, title);
    const ImVec2 ba(p0.x + 48 * k, p0.y + 40 * k + ts.y + 28 * k), bb(p1.x - 48 * k, ba.y + 18 * k);
    ui::progress_bar(dl, ba, bb, shown);
    char line[640];
    const double secs = double(plat::monotonic_ns() - g->job.started_ns) / 1e9;
    std::snprintf(line, sizeof line, "%s   %d%%   %d:%02d", what.c_str(), int(shown * 100 + 0.5f), int(secs) / 60,
                  int(secs) % 60);
    const ImVec2 ls = bf->CalcTextSizeA(sfs, FLT_MAX, 0, line);
    dl->AddText(bf, sfs, {(p0.x + p1.x - ls.x) * 0.5f, bb.y + 26 * k}, th.text_dim, line);
    const char* note = "This happens once, and again only after an update that needs it. It takes about 15 minutes.";
    const ImVec2 ns = bf->CalcTextSizeA(sfs * 0.9f, FLT_MAX, 0, note);
    dl->AddText(bf, sfs * 0.9f, {(p0.x + p1.x - ns.x) * 0.5f, bb.y + 26 * k + ls.y + 16 * k},
                ui::mix(th.text_off, th.text_dim, 0.4f), note);
}

void page_play() {
    rows::begin("##play", ImGui::GetContentRegionAvail().y);
    rows::section(lt(LtSuperWonders));
    const bool ready = g->status.ready == Ready::Yes;
    if (!ready) rows::disable_next();
    if (rows::action(lt(LtPlay), ready ? lt(LtStart) : lt(LtSetUpFirst),
                     ready ? "Start Super Mario Bros. Wonder." : "Your game files aren't set up yet: open Game files.",
                     true, ready))
        g->play = true;
    if (!ready && rows::action(lt(LtSetUpGameFiles), "", "Pick your ExeFS and RomFS folders and set the game up."))
        g->page = PageFiles, rows::focus_first();
    rows::section(lt(LtAbout));
    rows::action(lt(LtVersion), NX_VERSION_STRING,
                 "SuperWonders, a static recompilation of Super Mario Bros. Wonder (1.0.0 / 1.2.1) for PC. Nothing of the "
                 "game ships with it: it's set up from your own copy.",
                 true);
    rows::end();
}

// ---- the folder browser ----
std::string home_folder() {
    const char* h = std::getenv(
#if defined(_WIN32)
        "USERPROFILE"
#else
        "HOME"
#endif
    );
    return h && *h ? h : "";
}

void browse_open(int which) {
    const std::string& now = which == 1 ? g->config.exefs : which == 2 ? g->config.romfs : plat::data_root();
    std::error_code ec;
    std::string start = !now.empty() && fs::is_directory(u8(now), ec) ? str(u8(now).parent_path()) : home_folder();
    g->browse = which, g->browse_dir = start, g->browse_listed = "\x01", g->browse_inspected = "\x01";
    rows::focus_first();
    log("launcher: folder browser for the %s, from %s",
        which == 1 ? "ExeFS" : which == 2 ? "RomFS" : "Output", start.c_str());
}

void browse_close() {
    g->browse = 0;
    rows::focus_first();
}

void browse_read() {
    if (g->browse_listed == g->browse_dir) return;
    g->browse_listed = g->browse_dir;
    g->browse_list.clear();
    if (g->browse_dir.empty()) {  // the top: the drives, or the root
#if defined(_WIN32)
        const DWORD drives = GetLogicalDrives();
        for (int d = 0; d < 26; ++d)
            if (drives & (1u << d)) g->browse_list.push_back(std::string(1, char('A' + d)) + ":/");
#else
        g->browse_list.push_back("/");
#endif
        return;
    }
    std::error_code ec;
    for (auto it = fs::directory_iterator(u8(g->browse_dir), fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        const std::string n = str(it->path().filename());
        if (n.empty() || n[0] == '.' || n[0] == '$') continue;  // (hidden and system folders)
        g->browse_list.push_back(n);
        if (g->browse_list.size() >= 2000) break;
    }
    std::sort(g->browse_list.begin(), g->browse_list.end(), [](const std::string& a, const std::string& b) {
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
            const int x = std::tolower(static_cast<unsigned char>(a[i])), y = std::tolower(static_cast<unsigned char>(b[i]));
            if (x != y) return x < y;
        }
        return a.size() < b.size();
    });
}

void page_browse() {
    browse_read();
    const int mode = g->browse;
    const bool exefs = mode == 1;
    const bool romfs = mode == 2;
    const bool output = mode == 3;
    const std::string& dir = g->browse_dir;
    if (g->browse_inspected != dir) {
        g->browse_inspected = dir;
        if (exefs) g->browse_exefs_info = dir.empty() ? ExefsInfo{} : inspect_exefs(u8(dir));
        else if (romfs) g->browse_romfs_info = dir.empty() ? RomfsInfo{} : inspect_romfs(u8(dir));
        else if (output) g->browse_output_info = dir.empty() ? OutputInfo{} : inspect_output_dir(u8(dir), NX_GAMELIB_FILE);
    }
    rows::begin("##browse", ImGui::GetContentRegionAvail().y);
    rows::section(exefs ? lt(LtChooseExeFS) : romfs ? lt(LtChooseRomFS) : lt(LtChooseOutput));
    std::error_code ec;
    const bool ok = exefs   ? (folder_has(dir, "main") && folder_has(dir, "sdk"))
                    : romfs ? folder_has(dir, "Shader")
                            : (!dir.empty() && fs::is_directory(u8(dir), ec));
    const std::string where = dir.empty() ? std::string("This PC") : fit_path(dir, lt(LtUseFolder));
    std::string use_desc;
    if (!ok) {
        use_desc = exefs   ? "Not an ExeFS folder: it has no main and sdk."
                   : romfs ? "Not a RomFS folder: it has no Shader folder."
                           : "Choose an existing folder (or open a drive first).";
    } else if (exefs) {
        use_desc = "Use this ExeFS folder.\nDetected: " + g->browse_exefs_info.summary;
        if (!g->browse_exefs_info.build_id.empty())
            use_desc += "\nBuild ID: " + g->browse_exefs_info.build_id.substr(0, 16);
    } else if (romfs) {
        use_desc = "Use this RomFS folder.\nDetected: " + g->browse_romfs_info.summary;
    } else {
        if (g->browse_output_info.ready) {
            use_desc = "Use this output folder.\nAll compiled game files are already present" +
                       (g->browse_output_info.installed_version.empty()
                            ? std::string()
                            : " (" + g->browse_output_info.installed_version + ")") +
                       " - no recompilation needed.";
        } else {
            use_desc = "Use this output folder.\nCompiled files (game code, shaders, extracted RomFS, saves and "
                       "settings) will be saved here.";
        }
    }
    if (!dir.empty()) use_desc += "\n\n" + dir;
    if (!ok) rows::disable_next();
    if (rows::action(lt(LtUseFolder), where.c_str(), use_desc.c_str(), true, ok)) {
        if (output) {
            apply_output_folder(dir);
        } else {
            (exefs ? g->config.exefs : g->config.romfs) = dir;
            save_config(g->config);
            refresh_input_versions();
            log("launcher: %s folder %s (%s)", exefs ? "ExeFS" : "RomFS", dir.c_str(),
                exefs ? g->exefs_info.summary.c_str() : g->romfs_info.summary.c_str());
        }
        browse_close();
        rows::end();
        return;
    }
    if (!dir.empty() && rows::action(lt(LtUpFolder), "", "The folder above this one.", true)) {
        const fs::path p = u8(dir);
        g->browse_dir = p.has_parent_path() && p.parent_path() != p ? str(p.parent_path()) : "";
        if (g->browse_dir == dir) g->browse_dir.clear();  // (a drive's root: up to the drives)
        rows::focus_first();
    }
    if (rows::action(lt(LtHomeFolder), "", "Your home folder.", true)) g->browse_dir = home_folder(), rows::focus_first();
    rows::section(dir.empty() ? lt(LtDrives) : lt(LtFoldersHere));
    if (g->browse_list.empty()) rows::action("(no folders here)", "", "This folder has no folders in it.", false);
    std::string go;
    for (const std::string& n : g->browse_list)
        if (rows::action(n.c_str(), "", "Open this folder.", false)) go = n;
    if (!go.empty()) {
        g->browse_dir = dir.empty() ? go : str(u8(dir) / u8(go));
        rows::focus_first();
    }
    rows::section(lt(LtOtherWays));
    if (rows::action(lt(LtSystemPicker), "", "Choose the folder in the system's own window (a mouse helps).",
                     true)) {
        const char* title = exefs ? "Your ExeFS folder (main, sdk)"
                                  : romfs ? "Your RomFS folder" : "Choose the output data folder";
        plat::pick_folder(title, dir, [mode](std::string p) {
            std::lock_guard lk(g->pick_lock);
            if (mode == 1) g->picked_exefs = std::move(p);
            else if (mode == 2) g->picked_romfs = std::move(p);
            else g->picked_output = std::move(p);
        });
    }
    if (output && plat::data_root() != plat::default_data_root() &&
        rows::action(lt(LtResetDefault), "",
                     ("Switch back to the default folder:\n" + plat::default_data_root()).c_str(), true)) {
        apply_output_folder(plat::default_data_root());
        browse_close();
        rows::end();
        return;
    }
    if (rows::action(lt(LtCancel), "", "Back, without changing the folder (B).", true)) browse_close();
    rows::end();
}

void page_files() {
    if (g->browse) return page_browse();
    rows::begin("##files", ImGui::GetContentRegionAvail().y);
    rows::section(lt(LtYourGameFiles));
    const std::string cur_out = plat::data_root();
    const std::string out_val = fit_path(cur_out, lt(LtOutputFolder));
    std::string out_desc =
        "Where SuperWonders places everything compiled from ExeFS and RomFS (game code, shaders, extracted files), plus "
        "your saves and settings.\n"
        "If you select an output folder that already contains everything needed, you won't have to recompile.";
    if (g->status.ready == Ready::Yes) {
        out_desc += "\n\nStatus: Ready to play" +
                    (g->status.installed_version.empty() ? std::string() : " (Wonder " + g->status.installed_version + ")");
    } else {
        out_desc += "\n\nStatus: " + g->status.why;
    }
    out_desc += "\nNow: " + cur_out;
    if (rows::action(lt(LtOutputFolder), out_val.c_str(), out_desc.c_str(), true, g->status.ready == Ready::Yes))
        browse_open(3);

    const std::string exefs = g->config.exefs.empty() ? lt(LtChoose) : fit_path(g->config.exefs, lt(LtExeFS));
    std::string exefs_desc =
        "The ExeFS folder of your own copy of the game (versions 1.0.0 and 1.2.1 supported). It holds main and sdk.";
    if (!g->config.exefs.empty()) {
        exefs_desc += "\n\nVersion: " + g->exefs_info.summary;
        if (!g->exefs_info.build_id.empty()) exefs_desc += "\nBuild ID: " + g->exefs_info.build_id.substr(0, 16);
        exefs_desc += "\nNow: " + g->config.exefs;
    }
    if (rows::action(lt(LtExeFS), exefs.c_str(), exefs_desc.c_str(), true)) browse_open(1);

    const std::string romfs = g->config.romfs.empty() ? lt(LtChoose) : fit_path(g->config.romfs, lt(LtRomFSFolder));
    std::string romfs_desc = "The RomFS folder of your own copy of the game.";
    if (!g->config.romfs.empty()) {
        romfs_desc += "\n\nVersion: " + g->romfs_info.summary + "\nNow: " + g->config.romfs;
    }
    if (rows::action(lt(LtRomFSFolder), romfs.c_str(), romfs_desc.c_str(), true)) browse_open(2);

    std::string ver_val = g->pair_info.badge;
    std::string ver_desc = g->pair_info.detail;
    const bool has_inputs = !g->config.exefs.empty() || !g->config.romfs.empty();
    if (!has_inputs && g->status.ready == Ready::Yes) {
        ver_val = g->status.installed_version.empty() ? "Ready" : g->status.installed_version + " (Ready)";
        ver_desc = "The selected output folder already contains all compiled game files" +
                   (g->status.installed_version.empty() ? std::string() : " (" + g->status.installed_version + ")") +
                   ". No recompilation is needed - you can play immediately.";
    } else if (g->status.ready == Ready::Yes) {
        ver_desc += "\n\nOutput folder status: Ready" +
                    (g->status.installed_version.empty() ? std::string() : " (" + g->status.installed_version + ")") +
                    " - no recompilation needed unless you rebuild from new files.";
    }
    rows::action(lt(LtGameVersion), ver_val.c_str(), ver_desc.c_str(), true,
                 g->pair_info.state == PairState::Compatible || (!has_inputs && g->status.ready == Ready::Yes));

    const char* const modes[] = {lt(LtUseInPlace), lt(LtCopyIt)};
    // Both RomFS rows share a translated label, so give the storage mode its own ID scope.
    ImGui::PushID("romfs-storage-mode");
    rows::choice(lt(LtRomFSFolder), &g->config.romfs_copy, modes, 2, 1,
                 "Copy it: SuperWonders keeps its own copy (about 3.6 GB), so it keeps working if you move or delete "
                 "yours. Use it in place: no copy, but SuperWonders needs your RomFS folder where it is.");
    ImGui::PopID();

    rows::section(lt(LtSetUp));
    const bool ready = g->status.ready == Ready::Yes;
    if (ready && rows::action(lt(LtPlay), lt(LtStart),
                              "Everything required is already present in the selected output folder. Start the game "
                              "without recompiling.",
                              true, true))
        g->play = true;
    const bool pair_ok = g->pair_info.state == PairState::Compatible;
    std::string why;
    if (!pair_ok) {
        why = g->pair_info.detail;
        if (ready)
            why += "\n\nYour selected output folder is already ready to play without recompiling.";
    } else {
        why = "Builds the game code and translates shaders for Wonder " + g->exefs_info.version +
              " into the selected output folder. Steps that are already done are skipped.";
    }
    if (!pair_ok) rows::disable_next();
    if (rows::action(ready ? lt(LtSetUpAgain) : lt(LtSetUpGameFiles), "", why.c_str(), true, pair_ok && !ready))
        start_setup();
    if (rows::action(lt(LtOpenDataFolder), "",
                     ("Open the active output folder in your file manager:\n" + cur_out).c_str(), true))
        plat::open_folder(cur_out);
    rows::end();
}

// The active profile is chosen before boot. The legacy UID keeps the existing save path.
void profile_dialog(bool open) {
    static profiles::Database db;
    static int edited = 0;
    static int copy_from = 0, copy_to = 1;
    static char name[32] = {}, icon[1024] = {};
    static std::string message;
    static std::mutex image_lock;
    static std::string picked_image;
    auto read_fields = [&] {
        std::snprintf(name, sizeof name, "%s", db.users[edited].name.c_str());
        std::snprintf(icon, sizeof icon, "%s", db.users[edited].icon.c_str());
    };
    const float k = g_k;
    const ui::Theme& th = ui::theme();
    const ImGuiIO& io = ImGui::GetIO();
    const fs::path root = u8(plat::data_root());
    if (open) {
        db = profiles::load(root);
        edited = int(db.selected);
        copy_from = edited;
        copy_to = db.users.size() > 1 ? (edited == 0 ? 1 : 0) : 0;
        read_fields();
        message = db.valid ? "" : "Profile file is invalid or unreadable; it will not be overwritten.";
        ImGui::OpenPopup("##profiles_modal");
    }
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(io.DisplaySize.x * 0.92f, 1080 * k), 0}, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {32 * k, 28 * k});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * k);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10 * k);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {14 * k, 9 * k});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12 * k, 12 * k});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(th.card));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(ui::mix(th.card, th.accent, 0.45f)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(th.panel));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::ColorConvertU32ToFloat4(ui::mix(th.panel, th.accent, 0.18f)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImGui::ColorConvertU32ToFloat4(ui::mix(th.panel, th.accent, 0.28f)));
    ImGui::PushStyleColor(ImGuiCol_Header, ImGui::ColorConvertU32ToFloat4(ui::mix(th.panel, th.accent, 0.35f)));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::ColorConvertU32ToFloat4(ui::mix(th.panel, th.accent, 0.22f)));
    const bool modal_open = ImGui::BeginPopupModal(
        "##profiles_modal", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (!modal_open) {
        ImGui::PopStyleColor(7);
        ImGui::PopStyleVar(5);
        return;
    }

    auto end_profiles_modal = [&] {
        ImGui::EndPopup();
        ImGui::PopStyleColor(7);
        ImGui::PopStyleVar(5);
    };

    const ImVec4 builtin_colors[6] = {
        {0.27f, 0.53f, 0.90f, 1}, {0.94f, 0.47f, 0.37f, 1}, {0.35f, 0.71f, 0.55f, 1},
        {0.65f, 0.44f, 0.81f, 1}, {0.91f, 0.68f, 0.27f, 1}, {0.25f, 0.71f, 0.75f, 1},
    };
    const char* builtin_names[6] = {"Blue diamond", "Coral circle", "Green cross", "Purple diamond", "Gold circle", "Teal cross"};

    static std::unordered_map<std::string, uint64_t> icon_cache;
    auto draw_avatar = [&](ImDrawList* dl, ImVec2 center, float radius, const std::string& icon_str) {
        int bi = -1;
        if (std::strncmp(icon_str.c_str(), "builtin:", 8) == 0) {
            bi = std::atoi(icon_str.c_str() + 8);
            if (bi < 0 || bi >= 6) bi = -1;
        }
        if (bi < 0 && !icon_str.empty()) {
            auto it = icon_cache.find(icon_str);
            if (it == icon_cache.end()) {
                const auto rgba = profiles::decode_icon_rgba(u8(icon_str), 128);
                const uint64_t id = !rgba.empty() ? overlay::vk_upload_rgba_texture(rgba.data(), 128, 128) : 0;
                it = icon_cache.emplace(icon_str, id).first;
            }
            if (it->second) {
                dl->AddImageRounded((ImTextureID)(uintptr_t)it->second,
                                    {center.x - radius, center.y - radius},
                                    {center.x + radius, center.y + radius},
                                    {0, 0}, {1, 1}, IM_COL32_WHITE, radius);
                return;
            }
        }
        const ImU32 bg = bi >= 0 ? ImGui::ColorConvertFloat4ToU32(builtin_colors[bi])
                                 : (!icon_str.empty() ? th.accent : ui::mix(th.panel, th.text_off, 0.25f));
        dl->AddCircleFilled(center, radius, bg, 24);
        if (bi >= 0) {
            const float s = radius * 0.46f;
            const ImU32 fg = IM_COL32(255, 255, 255, 230);
            if (bi % 3 == 0) {  // diamond
                dl->AddQuadFilled({center.x, center.y - s}, {center.x + s, center.y},
                                  {center.x, center.y + s}, {center.x - s, center.y}, fg);
            } else if (bi % 3 == 1) {  // circle
                dl->AddCircleFilled(center, s * 0.78f, fg, 20);
            } else {  // cross
                const float t = s * 0.34f;
                dl->AddRectFilled({center.x - s, center.y - t}, {center.x + s, center.y + t}, fg, 2 * k);
                dl->AddRectFilled({center.x - t, center.y - s}, {center.x + t, center.y + s}, fg, 2 * k);
            }
        }
    };

    // Header
    ImGui::PushFont(ui::title());
    ImGui::SetWindowFontScale(0.66f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text));
    ImGui::TextUnformatted("Profiles");
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text_dim));
    ImGui::TextUnformatted("Choose the profile used when the game starts, or customize its name and icon.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 6 * k});

    {
        std::lock_guard lk(image_lock);
        if (!picked_image.empty()) {
            std::snprintf(icon, sizeof icon, "%s", picked_image.c_str());
            picked_image.clear();
        }
    }

    const float total_w = ImGui::GetContentRegionAvail().x;
    const float col_gap = 24 * k;
    const float left_w = std::floor(total_w * 0.32f);
    const float right_w = total_w - left_w - col_gap;

    // Left column: profile list + Add / Copy saves
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.accent2));
    ImGui::SetWindowFontScale(0.88f);
    ImGui::Text("LOCAL PROFILES (%zu/8)", db.users.size());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 4 * k});

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(th.panel));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12 * k);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12 * k, 12 * k});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * k, 8 * k});
    ImGui::BeginChild("##profile_list", {left_w, 316 * k}, ImGuiChildFlags_Borders);
    ImDrawList* list_dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < int(db.users.size()); ++i) {
        ImGui::PushID(i);
        const bool is_edited = i == edited;
        const bool is_active = i == int(db.selected);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float row_w = ImGui::GetContentRegionAvail().x;
        const float row_h = 52 * k;
        if (ImGui::InvisibleButton("##prof_row", {row_w, row_h})) {
            edited = i;
            read_fields();
            message.clear();
        }
        const bool hovered = ImGui::IsItemHovered();
        const ImU32 row_bg = is_edited ? ui::mix(th.card, th.accent, 0.26f)
                                       : (hovered ? ui::mix(th.card, th.text, 0.08f) : th.card);
        const ImU32 row_border = is_edited ? th.accent : ui::mix(th.panel, th.text_off, 0.25f);
        list_dl->AddRectFilled(p0, {p0.x + row_w, p0.y + row_h}, row_bg, 10 * k);
        list_dl->AddRect(p0, {p0.x + row_w, p0.y + row_h}, row_border, 10 * k, 0, is_edited ? 2.0f : 1.0f);
        draw_avatar(list_dl, {p0.x + 24 * k, p0.y + row_h * 0.5f}, 14 * k, db.users[i].icon);
        list_dl->AddText({p0.x + 48 * k, p0.y + (row_h - ImGui::GetFontSize()) * 0.5f}, th.text, db.users[i].name.c_str());
        if (is_active) {
            const char* tag = "ACTIVE";
            const ImVec2 ts = ImGui::CalcTextSize(tag);
            const float bw = ts.x + 16 * k;
            const float bh = 24 * k;
            const ImVec2 bp{p0.x + row_w - bw - 12 * k, p0.y + (row_h - bh) * 0.5f};
            list_dl->AddRectFilled(bp, {bp.x + bw, bp.y + bh}, ui::mix(th.panel, th.good, 0.25f), bh * 0.5f);
            list_dl->AddText({bp.x + 8 * k, bp.y + (bh - ts.y) * 0.5f}, th.good, tag);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    auto valid = [&] {
        if (!name[0]) { message = "Enter a name."; return false; }
        return true;
    };
    auto set_icon = [&](profiles::User& user) {
        if (!icon[0] || std::strncmp(icon, "builtin:", 8) == 0) { user.icon = icon; return true; }
        const auto bytes = profiles::import_icon(u8(icon));
        if (bytes.empty()) { message = "Could not read image (choose a JPEG or PNG)."; return false; }
        char id[33];
        std::snprintf(id, sizeof id, "%016llx%016llx", (unsigned long long)user.hi, (unsigned long long)user.lo);
        const fs::path dir = root / "user" / "icons";
        std::error_code ec; fs::create_directories(dir, ec);
        if (ec) { message = "Could not create icon directory."; return false; }
        const fs::path dest = dir / (std::string(id) + ".jpg");
        const fs::path tmp = dir / (std::string(id) + ".tmp");
        { std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
          if (!f || !f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())) || !f.flush()) {
              message = "Could not write profile icon."; return false;
          } }
#if defined(_WIN32)
        // MoveFileEx replaces the icon atomically without deleting the previous copy first.
        if (!MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) { message = "Could not save profile icon."; return false; }
#else
        fs::rename(tmp, dest, ec);
        if (ec) { message = "Could not save profile icon."; return false; }
#endif
        user.icon = str(dest);
        return true;
    };

    const bool at_limit = db.users.size() >= 8;
    if (at_limit) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(th.panel));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(ui::mix(th.panel, th.accent, 0.25f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(th.panel));
    if (ImGui::Button("+ Add profile", {left_w, 42 * k}) && valid()) {
        auto next = db;
        auto user = profiles::make_user(next, name);
        if (!set_icon(user)) { ImGui::PopStyleColor(3); ImGui::EndGroup(); end_profiles_modal(); return; }
        next.users.push_back(std::move(user));
        if (profiles::save(root, next)) {
            db = std::move(next); edited = int(db.users.size()) - 1; read_fields(); message = "Profile created; select it to play.";
        } else message = "Could not save profiles.";
    }
    if (at_limit) ImGui::EndDisabled();
    if (db.users.size() < 2) ImGui::BeginDisabled();
    if (ImGui::Button("Copy saves...", {left_w, 42 * k})) { message.clear(); ImGui::OpenPopup("##copy_saves_modal"); }
    if (db.users.size() < 2) ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    ImGui::EndGroup();

    ImGui::SameLine(0, col_gap);

    // Right column: edit selected profile
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.accent2));
    ImGui::SetWindowFontScale(0.88f);
    ImGui::TextUnformatted("PROFILE DETAILS");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 4 * k});

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(th.panel));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12 * k);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {22 * k, 20 * k});
    ImGui::BeginChild("##profile_editor", {right_w, 412 * k}, ImGuiChildFlags_Borders);

    // Preview + Name row
    {
        ImDrawList* ed_dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float av = 58 * k;
        ImGui::Dummy({av, av});
        draw_avatar(ed_dl, {p0.x + av * 0.5f, p0.y + av * 0.5f}, av * 0.45f, icon);
        ImGui::SameLine(0, 16 * k);
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text_dim));
        ImGui::TextUnformatted("Name");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(th.card));
        ImGui::InputText("##profile_name", name, sizeof name);
        ImGui::PopStyleColor();
        ImGui::EndGroup();
    }

    ImGui::Dummy({0, 10 * k});
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text_dim));
    const char* icon_label = "None";
    std::string icon_short;
    if (icon[0]) {
        if (std::strncmp(icon, "builtin:", 8) == 0) {
            int bi = std::atoi(icon + 8);
            icon_label = (bi >= 0 && bi < 6) ? builtin_names[bi] : icon;
        } else {
            icon_short = fs::path(u8(icon)).filename().string();
            icon_label = icon_short.empty() ? icon : icon_short.c_str();
        }
    }
    ImGui::Text("Custom image: %s", icon_label);
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(th.card));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(ui::mix(th.card, th.accent, 0.25f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(th.card));
    if (ImGui::Button("Choose image...", {210 * k, 40 * k}))
        plat::pick_image([](std::string path) {
            std::lock_guard lk(image_lock);
            picked_image = std::move(path);
        });
    ImGui::SameLine(0, 12 * k);
    if (ImGui::Button("No icon", {130 * k, 40 * k})) icon[0] = 0;
    ImGui::PopStyleColor(3);

    ImGui::Dummy({0, 10 * k});
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text_dim));
    ImGui::TextUnformatted("Original icons");
    ImGui::PopStyleColor();

    const float swatch_gap = 10 * k;
    const float swatch_w = (ImGui::GetContentRegionAvail().x - swatch_gap * 2.0f) / 3.0f;
    for (int i = 0; i < 6; ++i) {
        if (i && i % 3) ImGui::SameLine(0, swatch_gap);
        char expected[16];
        std::snprintf(expected, sizeof expected, "builtin:%d", i);
        const bool selected_icon = std::strcmp(icon, expected) == 0;
        const ImVec4 c = builtin_colors[i];
        ImGui::PushStyleColor(ImGuiCol_Button, {c.x * 0.35f, c.y * 0.35f, c.z * 0.35f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {c.x * 0.55f, c.y * 0.55f, c.z * 0.55f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, c);
        ImGui::PushStyleColor(ImGuiCol_Border, selected_icon ? c : ImVec4{c.x, c.y, c.z, 0.45f});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, selected_icon ? 2.0f : 1.0f);
        if (ImGui::Button(builtin_names[i], {swatch_w, 40 * k})) std::snprintf(icon, sizeof icon, "builtin:%d", i);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }

    ImGui::Dummy({0, 8 * k});
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text_off));
    ImGui::SetWindowFontScale(0.88f);
    ImGui::TextWrapped("Images are cropped to a square and converted to 256x256 JPEG.");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    // Footer: message + primary actions
    ImGui::Dummy({0, 10 * k});
    if (!message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.accent2));
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy({0, 6 * k});
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {20 * k, 10 * k});
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(th.accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(th.accent2));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(th.accent));
    if (ImGui::Button("Use this profile", {0, 44 * k})) {
        auto next = db;
        next.selected = size_t(edited);
        if (profiles::save(root, next)) db = std::move(next), message = "Selected for the next launch.";
        else message = "Could not save profiles.";
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, 12 * k);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(th.panel));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(ui::mix(th.panel, th.accent, 0.25f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(th.panel));
    if (ImGui::Button("Save changes", {0, 44 * k}) && valid()) {
        auto next = db;
        next.users[edited].name = name;
        if (!set_icon(next.users[edited])) { ImGui::PopStyleColor(3); ImGui::PopStyleVar(); end_profiles_modal(); return; }
        if (profiles::save(root, next)) db = std::move(next), message = "Profile saved.";
        else message = "Could not save profiles.";
    }
    ImGui::SameLine(0, 12 * k);
    if (ImGui::Button("Close", {0, 44 * k})) ImGui::CloseCurrentPopup();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    // Copy saves sub-modal
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(io.DisplaySize.x * 0.80f, 520 * k), 0}, ImGuiCond_Always);
    if (ImGui::BeginPopupModal("##copy_saves_modal", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(ui::title());
        ImGui::SetWindowFontScale(0.60f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text));
        ImGui::TextUnformatted("Copy saves");
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
        ImGui::Dummy({0, 6 * k});
        auto choose = [&](const char* label, int& value) {
            if (ImGui::BeginCombo(label, db.users[value].name.c_str())) {
                for (int i = 0; i < int(db.users.size()); ++i) {
                    ImGui::PushID(i);
                    if (ImGui::Selectable(db.users[i].name.c_str(), i == value)) value = i;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        };
        choose("From", copy_from);
        choose("To", copy_to);
        ImGui::Dummy({0, 4 * k});
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text_dim));
        ImGui::TextWrapped("Close any running game before copying. The destination's existing save will be backed up.");
        ImGui::PopStyleColor();
        ImGui::Dummy({0, 10 * k});
        if (copy_from == copy_to) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(th.accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(th.accent2));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(th.accent));
        if (ImGui::Button("Confirm copy", {0, 40 * k})) {
            fs::path backup;
            std::string error;
            if (profiles::copy_save(root, db.users[copy_from], db.users[copy_to], backup, error)) {
                message = "Save copied from " + db.users[copy_from].name + " to " + db.users[copy_to].name + ".";
                if (!backup.empty()) message += " Backup: " + str(backup);
                ImGui::CloseCurrentPopup();
            } else message = error;
        }
        ImGui::PopStyleColor(3);
        if (copy_from == copy_to) ImGui::EndDisabled();
        ImGui::SameLine(0, 10 * k);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(th.panel));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(ui::mix(th.panel, th.accent, 0.25f)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(th.panel));
        if (ImGui::Button("Cancel", {0, 40 * k})) ImGui::CloseCurrentPopup();
        ImGui::PopStyleColor(3);
        ImGui::EndPopup();
    }
    end_profiles_modal();
}

void page_settings() {
    rows::begin("##settings", ImGui::GetContentRegionAvail().y);
    rows::section(lt(LtSettings));
    const bool open_profiles = rows::action("Profiles", "", "Create and select local profiles and set their names or JPEG icons.", true);
    static constexpr const char* kAspectText[13][3] = {
        {"Aspect ratio", "Takes effect the next time the game starts.", "16:9 (original)"},
        {"Formato schermo", "Si applica al prossimo avvio del gioco.", "16:9 (originale)"},
        {"Seitenverhältnis", "Wird beim nächsten Spielstart wirksam.", "16:9 (Original)"},
        {"Format d'image", "S'applique au prochain lancement du jeu.", "16:9 (original)"},
        {"Relación de aspecto", "Se aplica la próxima vez que inicie el juego.", "16:9 (original)"},
        {"화면 비율", "게임을 다시 시작하면 적용됩니다.", "16:9 (원본)"},
        {"アスペクト比", "次回ゲーム起動時に適用されます。", "16:9 (オリジナル)"},
        {"نسبة العرض إلى الارتفاع", "يتم تطبيقه عند تشغيل اللعبة في المرة القادمة.", "16:9 (أصلي)"},
        {"Соотношение сторон", "Вступит в силу при следующем запуске игры.", "16:9 (оригинал)"},
        {"En-boy oranı", "Oyun bir sonraki başlatıldığında geçerli olur.", "16:9 (özgün)"},
        {"Beeldverhouding", "Wordt toegepast bij de volgende keer dat het spel start.", "16:9 (origineel)"},
        {"画面比例", "将在下次启动游戏时生效。", "16:9 (原始)"},
        {"畫面比例", "將在下次啟動遊戲時生效。", "16:9 (原始)"},
    };
    const int lang = std::clamp(overlay::menu_language(), 0, 12);
    const char* aspect_names[] = {kAspectText[lang][2], "21:9", "32:9"};
    int aspect = overlay::aspect_ratio();
    if (rows::choice(kAspectText[lang][0], &aspect, aspect_names, 3, 0, kAspectText[lang][1]))
        overlay::set_aspect_ratio(aspect);
    if (rows::action(lt(LtOpenSettings), "", "Display, graphics, audio, controls and the overlay: the same settings as "
                                          "the in-game menu (F1, or Minus + Plus held). Saved in pc-settings.ini.",
                     true, true))
        overlay::toggle();
    rows::end();
    profile_dialog(open_profiles);
}

void page_mods() {
    rows::begin("##mods", ImGui::GetContentRegionAvail().y);
    rows::section(lt(LtMods));
    if (g->mods.empty())
        rows::action("No mods yet", "", "Put each mod in a folder of its own in the mods folder. A mod holds a romfs "
                                        "folder (romfs/, or contents/<title id>/romfs/ as Switch mods come).",
                     true);
    for (Mod& m : g->mods) {
        bool on = m.on;
        if (rows::toggle(m.name.c_str(), &on, true,
                         "Its files go over the game's. Mods load in name order; the first with a file wins. "
                         "Applies the next time the game starts.") &&
            set_mod(m, on))
            m.on = on;
    }
    rows::section(lt(LtFolder));
    if (rows::action(lt(LtOpenModsFolder), "", "Add, remove or rename mods there.", true))
        plat::open_folder(plat::data_root() + "/mods");
    if (rows::action(lt(LtRefresh), "", "Read the mods folder again.", true)) g->mods = list_mods();
    rows::end();
}

void page_quit() {
    rows::begin("##quit", ImGui::GetContentRegionAvail().y);
    rows::section(lt(LtQuit));
    if (rows::action(lt(LtQuitSuperWonders), "", "Close the launcher.", true)) g->quit = true;
    rows::end();
}

void low_vram_warning_dialog(float w, float h, float k) {
    struct LowVramText {
        const char* title;
        const char* body;
        const char* activate;
        const char* dismiss;
    };
    static constexpr LowVramText kLowVramText[13] = {
        {"Low VRAM Detected",
         "Your GPU has less than 6 GB of VRAM and Low VRAM Mode is currently off. You may experience crashes, "
         "slowdowns, or graphical glitches. Activating Low VRAM Mode is recommended.",
         "Activate Low VRAM Mode", "Dismiss"},
        {"VRAM insufficiente rilevata",
         "La tua GPU ha meno di 6 GB di VRAM e la modalita Low VRAM non e attiva. Potresti riscontrare crash, "
         "rallentamenti o glitch grafici. Si consiglia di attivarla.",
         "Attiva modalita Low VRAM", "Ignora"},
        {"Wenig VRAM erkannt",
         "Deine GPU hat weniger als 6 GB VRAM und der Low-VRAM-Modus ist aus. Es kann zu Absturzen, Rucklern oder "
         "Grafikfehlern kommen. Es wird empfohlen, ihn zu aktivieren.",
         "Low-VRAM-Modus aktivieren", "Schliessen"},
        {"VRAM faible detectee",
         "Votre GPU dispose de moins de 6 Go de VRAM et le mode Low VRAM est desactive. Vous pouvez rencontrer des "
         "crashs, ralentissements ou bugs graphiques. Il est recommande de l'activer.",
         "Activer le mode Low VRAM", "Ignorer"},
        {"VRAM baja detectada",
         "Tu GPU tiene menos de 6 GB de VRAM y el modo Low VRAM esta desactivado. Puedes sufrir cierres, "
         "ralentizaciones o fallos graficos. Se recomienda activarlo.",
         "Activar modo Low VRAM", "Descartar"},
        {"VRAM 적음",
         "GPU VRAM이 6 GB 이하고 저 VRAM 모드가 사용 안 됨입니다. 강제 종료, 속도 저하 또는 화면 이상이 있을 수 "
         "있습니다. 저 VRAM 모드 사용이 필요합니다.",
         "저 VRAM 모드 사용", "닫기"},
        {"VRAMが少ないです",
         "GPUのVRAMが6 GB以下で、低VRAMモードがオフです。動作低下、画面の乱れ、強制終了が起きることがあります。"
         "低VRAMモードを使用してください。",
         "低VRAMモードを使用", "閉じる"},
        {"تم اكتشاف VRAM منخفض",
         "بطاقة الرسومات لديك تحتوي على أقل من 6 جيجابايت من VRAM ووضع VRAM المنخفض غير مفعل. قد تواجه أعطالاً أو "
         "بطئاً أو أخطاءً رسومية. يُنصح بتفعيله.",
         "تفعيل وضع VRAM المنخفض", "تجاهل"},
        {"Мало видеопамяти (VRAM)",
         "У вашей видеокарты менее 6 ГБ VRAM, а режим Low VRAM выключен. Возможны вылеты, тормоза и графические "
         "сбои. Рекомендуется включить этот режим.",
         "Включить режим Low VRAM", "Закрыть"},
        {"Dusuk VRAM Algilandi",
         "GPU'nuzda 6 GB'tan az VRAM var ve Dusuk VRAM Modu kapali. Cokmeler, yavaslamalar veya grafik hatalari "
         "yasayabilirsiniz. Etkinlestirmeniz onerilir.",
         "Dusuk VRAM Modunu Etkinlestir", "Kapat"},
        {"Weinig VRAM gedetecteerd",
         "Je GPU heeft minder dan 6 GB VRAM en de Low VRAM-modus staat uit. Je kunt crashes, vertragingen of "
         "grafische fouten ervaren. Activeren wordt aanbevolen.",
         "Low VRAM-modus activeren", "Negeren"},
        {"VRAM 不足",
         "GPU 的 VRAM 不足 6 GB, 低 VRAM 模式未开启. 游玩时可能出现画面异常或变慢, 请开启低 VRAM 模式.",
         "开启低 VRAM 模式", "关闭"},
        {"VRAM 不足",
         "GPU 的 VRAM 不足 6 GB, 低 VRAM 模式未開啟. 遊玩時可能出現畫面異常或變慢, 請開啟低 VRAM 模式.",
         "開啟低 VRAM 模式", "關閉"},
    };

    static bool checked = false;
    static bool should_open = false;
    const fs::path marker = u8(plat::data_root()) / "low_vram_prompt_seen";
    if (!checked) {
        const uint64_t vram = nvn::gpu_vram_bytes();
        if (vram > 0) {
            checked = true;
            constexpr uint64_t kSixGiB = 6ull * 1024ull * 1024ull * 1024ull;
            if (vram < kSixGiB && !overlay::low_vram_profile() && !file(marker)) should_open = true;
        }
    }
    if (should_open) {
        ImGui::OpenPopup("##low_vram_warning");
        should_open = false;
    }

    const int lang = std::clamp(overlay::menu_language(), 0, 12);
    const LowVramText& t = kLowVramText[lang];
    const ui::Theme& th = ui::theme();
    ImGui::SetNextWindowPos({w * 0.5f, h * 0.5f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(w * 0.85f, 660 * k), 0}, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {28 * k, 24 * k});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14 * k);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8 * k);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(th.card));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(th.warn));
    const bool open = ImGui::BeginPopupModal(
        "##low_vram_warning", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (open) {
        auto mark_seen = [&] {
            if (FILE* f = plat::fopen_utf8(str(marker), "wb")) {
                std::fwrite("1\n", 1, 2, f);
                std::fclose(f);
            }
        };
        ImGui::PushFont(ui::title());
        ImGui::SetWindowFontScale(0.62f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.warn));
        ImGui::TextUnformatted(t.title);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
        ImGui::Dummy({0, 8 * k});
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text));
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 28 * k);
        ImGui::TextUnformatted(t.body);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy({0, 18 * k});

        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(th.accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(th.accent2));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(th.accent));
        if (ImGui::Button(t.activate, {0, 44 * k})) {
            overlay::set_low_vram_profile(true);
            mark_seen();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0, 12 * k);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(th.panel));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(th.card));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(th.panel));
        if (ImGui::Button(t.dismiss, {0, 44 * k})) {
            mark_seen();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

struct LangWarningText {
    const char* title;
    const char* body_fmt;
    const char* dismiss;
};
constexpr LangWarningText kLangWarningText[13] = {
    {"Unsupported Game Language",
     "Thai and Polski are only supported in Super Mario Bros. Wonder 1.2.1, not in your selected version (%s).\n\n"
     "Please choose another game language in Settings, or set up version 1.2.1.",
     "Dismiss"},
    {"Lingua di gioco non supportata",
     "Le lingue Thai e Polski sono supportate solo nella versione 1.2.1 di Super Mario Bros. Wonder, non nella "
     "versione selezionata (%s).\n\nSeleziona un'altra lingua di gioco nelle Impostazioni oppure configura la "
     "versione 1.2.1.",
     "Ignora"},
    {"Nicht unterstutzte Spielsprache",
     "Thai und Polski werden nur in Super Mario Bros. Wonder 1.2.1 unterstutzt, nicht in der ausgewahlten Version "
     "(%s).\n\nBitte wahle in den Einstellungen eine andere Spielsprache oder richte Version 1.2.1 ein.",
     "Schliessen"},
    {"Langue du jeu non prise en charge",
     "Thai et Polski ne sont pris en charge que dans Super Mario Bros. Wonder 1.2.1, pas dans votre version "
     "selectionnee (%s).\n\nVeuillez choisir une autre langue dans les Parametres ou configurer la version 1.2.1.",
     "Ignorer"},
    {"Idioma del juego no compatible",
     "Thai y Polski solo son compatibles con Super Mario Bros. Wonder 1.2.1, no con la version seleccionada (%s).\n\n"
     "Elige otro idioma del juego en Ajustes o configura la version 1.2.1.",
     "Descartar"},
    {"지원되지 않는 게임 언어",
     "Thai 및 Polski 언어는 Super Mario Bros. Wonder 1.2.1에서만 지원되며, 현재 선택된 버전(%s)에서는 지원되지 "
     "않습니다.\n\n설정에서 다른 게임 언어를 선택하거나 1.2.1 버전을 설정해 주세요.",
     "닫기"},
    {"未対応のゲーム言語",
     "Thai および Polski は Super Mario Bros. Wonder 1.2.1 のみでサポートされており、選択中のバージョン (%s) では"
     "利用できません。\n\n設定で別のゲーム言語を選ぶか、1.2.1 をセットアップしてください。",
     "閉じる"},
    {"لغة اللعبة غير مدعومة",
     "تتوفر لغتا Thai وPolski في Super Mario Bros. Wonder 1.2.1 فقط، وليستا مدعومتين في الإصدار المحدد (%s).\n\n"
     "يرجى اختيار لغة أخرى من الإعدادات أو إعداد الإصدار 1.2.1.",
     "تجاهل"},
    {"Язык игры не поддерживается",
     "Языки Thai и Polski поддерживаются только в Super Mario Bros. Wonder 1.2.1 и не работают в выбранной версии "
     "(%s).\n\nВыберите другой язык игры в настройках или установите версию 1.2.1.",
     "Закрыть"},
    {"Desteklenmeyen Oyun Dili",
     "Thai ve Polski yalnizca Super Mario Bros. Wonder 1.2.1 surumunde desteklenir; secili surumde (%s) desteklenmez."
     "\n\nLutfen Ayarlar'dan baska bir oyun dili secin veya 1.2.1 surumunu kurun.",
     "Kapat"},
    {"Niet-ondersteunde speltaal",
     "Thai en Polski worden alleen ondersteund in Super Mario Bros. Wonder 1.2.1, niet in de geselecteerde versie "
     "(%s).\n\nKies een andere speltaal in Instellingen of stel versie 1.2.1 in.",
     "Negeren"},
    {"不支持的游戏语言",
     "Thai 和 Polski 仅支持 Super Mario Bros. Wonder 1.2.1，不支持当前所选版本 (%s)。\n\n"
     "请在设置中选择其他游戏语言，或配置 1.2.1 版本。",
     "关闭"},
    {"不支援的遊戲語言",
     "Thai 與 Polski 僅支援 Super Mario Bros. Wonder 1.2.1，不支援目前所選版本 (%s)。\n\n"
     "請在設定中選擇其他遊戲語言，或配置 1.2.1 版本。",
     "關閉"},
};

void unsupported_language_warning_dialog(float w, float h, float k) {
    if (g->open_lang_warning) {
        ImGui::OpenPopup("##unsupported_game_language_warning");
        g->open_lang_warning = false;
    }

    const int lang = std::clamp(overlay::menu_language(), 0, 12);
    const LangWarningText& t = kLangWarningText[lang];
    const ui::Theme& th = ui::theme();
    ImGui::SetNextWindowPos({w * 0.5f, h * 0.5f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(w * 0.85f, 660 * k), 0}, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {28 * k, 24 * k});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14 * k);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8 * k);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(th.card));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(th.warn));
    const bool open = ImGui::BeginPopupModal(
        "##unsupported_game_language_warning", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (open) {
        const char* ver = g->status.installed_version.empty() ? "unknown" : g->status.installed_version.c_str();
        char body[512];
        std::snprintf(body, sizeof body, t.body_fmt, ver);

        ImGui::PushFont(ui::title());
        ImGui::SetWindowFontScale(0.62f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.warn));
        ImGui::TextUnformatted(t.title);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
        ImGui::Dummy({0, 8 * k});
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text));
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 28 * k);
        ImGui::TextUnformatted(body);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy({0, 18 * k});

        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(th.accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(th.accent2));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(th.accent));
        if (ImGui::Button(t.dismiss, {0, 44 * k})) ImGui::CloseCurrentPopup();
        ImGui::PopStyleColor(3);
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void screen(float w, float h) {
    if (!g) return;
    if (g->follow_hold > 0) g->follow_hold--;
    g_k = std::clamp(h / 1080.0f, 0.5f, 3.0f);
    const float k = g_k;
    const ui::Theme& th = ui::theme();
    {
        std::string picked_exefs, picked_romfs, picked_output;
        {
            std::lock_guard lk(g->pick_lock);  // folders the picker returned
            picked_exefs = std::move(g->picked_exefs);
            picked_romfs = std::move(g->picked_romfs);
            picked_output = std::move(g->picked_output);
            g->picked_exefs.clear(), g->picked_romfs.clear(), g->picked_output.clear();
        }
        if (!picked_exefs.empty() || !picked_romfs.empty() || !picked_output.empty())
            g->browse = 0;  // (the system's picker answered)
        if (!picked_output.empty()) apply_output_folder(picked_output);
        if (!picked_exefs.empty() || !picked_romfs.empty()) {
            if (!picked_exefs.empty()) g->config.exefs = picked_exefs;
            if (!picked_romfs.empty()) g->config.romfs = picked_romfs;
            save_config(g->config);
            refresh_input_versions();
        }
    }
    static bool was_running = false;
    if (was_running && !g->job.running) {  // a setup finished
        refresh_status();
        const int r = g->job.result;
        g->note = r == 0 ? "Your game is set up." : g->job.was_cancelled() ? "Setup cancelled." : "Setup failed.";
        log("launcher: setup %s (exit %d)", r == 0 ? "finished" : "failed", r);
        if (r == 0 && !g->config_mode) g->play = true;  // (a first setup goes straight into the game)
        if (r == 0) g->page = PagePlay, rows::focus_first();
    }
    was_running = g->job.running;

    const ImVec2 ds{w, h};
    backdrop(ImGui::GetBackgroundDrawList(), ds);
    // fades and rises in, as the menu does
    const float reveal = ui::smoothstep(std::min(1.0f, float(double(plat::monotonic_ns() - g->shown_ns) / 0.35e9)));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, reveal);
    ImGui::SetNextWindowPos({0, (1 - reveal) * 20 * k});
    ImGui::SetNextWindowSize(ds);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus;
    // The launcher has the controller whenever the settings menu is closed (it closes: back where it was).
    static bool menu_was_open = false;
    const bool menu_open = overlay::is_open();
    if (!menu_open && (menu_was_open || !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))) ImGui::SetNextWindowFocus();
    if (menu_was_open && !menu_open) {
        if (g->in_list) g->focus_list = true;
        else rows::focus_first();
    }
    menu_was_open = menu_open;
    ImGui::Begin("##launcher", nullptr, f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float margin = 52 * k, nav_w = std::clamp(w * 0.22f, 260 * k, 320 * k);

    // Header: brand on the left, status pill + theme toggle button on the right, separated by a clean 1px rule.
    ImGui::SetCursorPos({margin, margin * 0.68f});
    ImGui::BeginGroup();
    ImGui::PushFont(ui::title());
    ImGui::SetWindowFontScale(1.05f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text));
    ImGui::TextUnformatted("SUPERWONDERS");
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text_dim));
    ImGui::SetWindowFontScale(0.86f);
    ImGui::TextUnformatted(g->config_mode ? "Super Mario Bros. Wonder  ·  Configuration" : "Super Mario Bros. Wonder");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    const float header_bottom = ImGui::GetItemRectMax().y;

    {
        const Ready r = g->status.ready;
        const ImU32 sc = r == Ready::Yes ? th.good : r == Ready::Rebuild ? th.warn : th.bad;
        const char* st = r == Ready::Yes ? lt(LtFilesReady) : r == Ready::Rebuild ? lt(LtNeedsRebuild) : lt(LtFilesNotSetUp);
        const float pill_h = 38 * k, btn_fs = ImGui::GetFontSize() * 0.84f;
        const ImVec2 sts = ui::body()->CalcTextSizeA(btn_fs, FLT_MAX, 0, st);
        const ImVec2 tts = ui::body()->CalcTextSizeA(btn_fs, FLT_MAX, 0, th.name);
        const float status_w = 14 * k * 2 + 10 * k + 8 * k + sts.x;
        const float theme_w = 14 * k * 2 + 11 * k + 8 * k + tts.x;
        const float right_y = margin * 0.78f;
        const float theme_x = w - margin - theme_w;
        const float status_x = theme_x - 12 * k - status_w;
        dl->AddRectFilled({status_x, right_y}, {status_x + status_w, right_y + pill_h},
                          ui::fade(th.card, 0.75f), pill_h * 0.5f);
        dl->AddRect({status_x, right_y}, {status_x + status_w, right_y + pill_h},
                    ui::fade(IM_COL32(255, 255, 255, 22)), pill_h * 0.5f, 0, std::max(1.0f, k));
        dl->AddCircleFilled({status_x + 14 * k + 5 * k, right_y + pill_h * 0.5f}, 5 * k, sc);
        dl->AddText(ui::body(), btn_fs, {status_x + 14 * k + 10 * k + 8 * k, right_y + (pill_h - sts.y) * 0.5f},
                    ui::fade(th.text_dim), st);
        ImGui::SetCursorPos({theme_x, right_y});
        if (ui::theme_button(k)) overlay::set_theme_index(ui::theme_index());
    }

    const float divider_y = header_bottom + 18 * k;
    dl->AddLine({margin, divider_y}, {w - margin, divider_y}, ui::fade(IM_COL32(255, 255, 255, 18)), std::max(1.0f, k));
    const float top = divider_y + 24 * k;

    // Sidebar navigation: clean Sentence/Title Case labels with left accent indicator, matching the settings rows.
    const bool busy = g->job.running || g->demo_progress;
    ImGui::SetCursorPos({margin, top});
    ImGui::BeginGroup();
    bool list_focused = false;
    for (int i = 0; i < kPages; ++i) {
        ImGui::PushID(i);
        const bool sel = g->page == i;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float row_h = 54 * k;
        const float a = ui::eased(ImGui::GetID("hl"), sel ? 1.0f : 0.0f, 16.0f);
        if (a > 0.01f) {
            dl->AddRectFilled(at, {at.x + nav_w, at.y + row_h}, ui::fade(IM_COL32(255, 255, 255, 18), a), 10 * k);
            const float bh = row_h * 0.52f;
            dl->AddRectFilled({at.x, at.y + (row_h - bh) * 0.5f}, {at.x + 3.5f * k, at.y + (row_h + bh) * 0.5f},
                              ui::fade(th.accent, a), 2 * k);
        }
        const bool disabled = busy && i != g->page;
        if (disabled) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ui::fade(IM_COL32(255, 255, 255, 10)));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ui::fade(IM_COL32(255, 255, 255, 16)));
        char label[64];
        std::snprintf(label, sizeof label, "   %s", lt(static_cast<LauncherText>(LtPlay + i)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(sel ? th.text : th.text_dim));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        if (sel && g->focus_list && !menu_open) ImGui::SetKeyboardFocusHere(), g->focus_list = false;
        const bool clicked = ImGui::Selectable(label, sel, ImGuiSelectableFlags_None, {nav_w, row_h});
        ImGui::PopStyleVar();
        const bool focused = ImGui::IsItemFocused();
        list_focused |= focused;
        if (focused && i != g->page && !g->follow_hold)  // (the page follows the cursor)
            g->page = i, g->browse = 0, log("launcher: page %s", kPageNames[i]);
        if (clicked) {  // A (or a click): into the page
            if (i != g->page) g->page = i, g->browse = 0, log("launcher: page %s", kPageNames[i]);
            rows::focus_first();
        }
        if (focused && !menu_open) {
            const ImVec2 b0 = ImGui::GetItemRectMin(), b1 = ImGui::GetItemRectMax();
            dl->AddRect(b0, b1, ui::fade(th.accent, 0.7f), 10 * k, 0, std::max(1.0f, 1.4f * k));
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleColor(3);
        if (disabled) ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndGroup();
    g->in_list = list_focused;

    // Detailed status note, lower left
    {
        const std::string& note = !g->note.empty() ? g->note : g->status.why;
        if (!note.empty()) {
            const float y = h - margin - 92 * k;
            ImGui::SetCursorPos({margin + 8 * k, y});
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.text_off));
            ImGui::SetWindowFontScale(0.86f);
            ImGui::PushTextWrapPos(margin + nav_w - 8 * k);
            ImGui::TextUnformatted(note.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
        }
    }

    // the page, on the right
    const float px = margin + nav_w + 40 * k, pw = w - px - margin, ph = h - top - margin - 64 * k;
    ImGui::SetCursorPos({px, top});
    if (busy || (g->job.result > 0 && !g->job.was_cancelled() && g->page == PageFiles)) {
        ImGui::BeginChild("##job", {pw, ph}, ImGuiChildFlags_NavFlattened);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float card_h = 300 * k;
        if (busy) {
            progress_card(dl, p0, {p0.x + pw, p0.y + card_h});
            ImGui::SetCursorPosY(card_h + 30 * k);
            rows::begin("##cancel", ph - card_h - 30 * k);
            if (rows::action("Cancel", "", "Stop the setup. What it finished is kept, and it carries on from there "
                                           "next time.", true))
                g->job.cancel();
            rows::end();
        } else {  // the last setup failed: what it said
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(th.bad));
            ImGui::TextUnformatted("The setup stopped with an error:");
            ImGui::PopStyleColor();
            std::vector<std::string> tail;
            {
                std::lock_guard lk(g->job.lock);
                tail = g->job.tail;
            }
            ImGui::PushTextWrapPos(pw);
            for (const std::string& l : tail) ImGui::TextUnformatted(l.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy({0, 20 * k});
            rows::begin("##failed", 200 * k);
            if (rows::action("Back", "", "Check the folders and try again.", true)) g->job.result = -1;
            rows::end();
        }
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("##page", {pw, ph}, ImGuiChildFlags_NavFlattened);
        switch (g->page.load()) {
            case PagePlay: page_play(); break;
            case PageFiles: page_files(); break;
            case PageSettings: page_settings(); break;
            case PageMods: page_mods(); break;
            case PageUpdates: page_updates(g_k); break;
            default: page_quit(); break;
        }
        ImGui::EndChild();
    }

    // the controls, along the bottom
    const ui::Hint hints[] = {{"A", lt(LtSelect)}, {"B", lt(LtBack)}, {"L", ""}, {"R", lt(LtPages)}, {"+", lt(LtPlayHint)}};
    ui::hints(dl, w * 0.5f, h - margin * 0.7f, 34 * k, hints, 5);
    low_vram_warning_dialog(w, h, k);
    unsupported_language_warning_dialog(w, h, k);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // The controller, by the player's own bindings (the pad as the overlay saw it: NpadButton bits), and the keys.
    const uint64_t pad = overlay::pad_buttons(), pressed = pad & ~g->pad_before;
    g->pad_before = pad;
    // A modal or active text field owns the input; Backspace and mapped controller keys must not move launcher focus.
    if (menu_open || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) || ImGui::GetIO().WantTextInput) return;
    const bool right = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
    const bool back = (pressed & plat::kB) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    if (list_focused && right) rows::focus_first();  // right: into the page
    else if (!list_focused && back) {                // B: the browser closes, else back to the page list
        if (g->browse) browse_close();
        else g->focus_list = true;
    }
    if (!busy && (pressed & (plat::kL | plat::kR))) {  // L / R: the page before / after
        const int step = (pressed & plat::kR) ? 1 : kPages - 1;
        g->page = (g->page + step) % kPages, g->browse = 0, g->focus_list = true, g->follow_hold = 3;
        log("launcher: page %s", kPageNames[g->page.load()]);
    }
    if (!busy && g->status.ready == Ready::Yes && ((pressed & plat::kPlus) || ImGui::IsKeyPressed(ImGuiKey_F5, false)))
        g->play = true;  // +: play
}

bool wanted(const std::vector<std::string>& args, bool& config_mode) {
    auto has = [&](const char* a) { return std::find(args.begin(), args.end(), a) != args.end(); };
    config_mode = has("--config");
    if (has("--play")) return false;
#if defined(_WIN32)
    // Release packages contain two copies of the runtime with distinct entry points.
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (length && length < path.size()) {
        path.resize(length);
        const auto name = fs::path(path).filename().wstring();
        if (_wcsicmp(name.c_str(), L"wonder.exe") == 0 && !config_mode) return false;
    }
#else
    const fs::path self = self_exe_path();
    if (!self.empty() && self.filename() == "wonder" && !config_mode && file(self.parent_path() / "wonder-launcher"))
        return false;
#endif
    auto env_on = [](const char* n) {
        const char* e = std::getenv(n);
        return e && *e && std::strcmp(e, "0") != 0;
    };
    if (env_on("NX_NO_LAUNCHER") || env_on("NX_HEADLESS") || env_on("NX_INPUT_SCRIPT") || env_on("NX_INPUT_REPLAY"))
        return config_mode && !env_on("NX_HEADLESS");
    return true;
}

}  // namespace

void prepare_ultrawide_layouts() {
    const char* profile = std::getenv("NX_ASPECT_PROFILE");
    if (!profile || (std::strcmp(profile, "21:9") != 0 && std::strcmp(profile, "32:9") != 0)) return;
    const Kit kit = find_kit();
    fs::path root = u8(kit.script).parent_path();
    fs::path script = root / "tools" / "wonder_uw_prepare.py";
    if (!file(script)) script = root / "wonder_uw_prepare.py";
    if (kit.python.empty() || !file(script)) {
        log("ultrawide: missing layout preparation tools");
#if defined(_WIN32)
        MessageBoxA(nullptr, "Ultrawide setup tools are missing. Repair or reinstall the game-build kit.",
                    "SuperWonders - Ultrawide setup", MB_OK | MB_ICONERROR);
#endif
        plat::exit_process(1);
    }
    Job job;
    job.start({kit.python, "-u", str(script), "--data-root", plat::data_root()});
    while (job.running) plat::sleep_ns(50'000'000);
    if (job.result != 0) {
        log("ultrawide: layout preparation failed (%d)", int(job.result));
#if defined(_WIN32)
        MessageBoxA(nullptr, "Could not prepare ultrawide layouts from your game files. See data/nx.log for details.",
                    "SuperWonders - Ultrawide setup", MB_OK | MB_ICONERROR);
#endif
        plat::exit_process(1);
    }
}

void run() {
    if (!overlay::enabled()) return;
    bool config_mode = false;
    if (!wanted(plat::program_args(), config_mode)) {
#if defined(_WIN32)
        // Direct game launch should explain missing setup before booting the guest.
        const char* headless = std::getenv("NX_HEADLESS");
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if ((!headless || !*headless || std::strcmp(headless, "0") == 0) && length && length < path.size()) {
            path.resize(length);
            if (_wcsicmp(fs::path(path).filename().c_str(), L"wonder.exe") == 0) {
                const Status status = check_status(find_kit());
                overlay::set_game_version(status.installed_version);
                if (status.ready != Ready::Yes) {
                    log("game: cannot start: %s", status.why.c_str());
                    const std::string message = status.why +
                        "\n\nOpen wonder-launcher.exe, select your game files and complete Set up game files.";
                    MessageBoxA(nullptr, message.c_str(), "SuperWonders - Game setup required", MB_OK | MB_ICONINFORMATION);
                    plat::exit_process(1);
                }
                if (overlay::game_language() >= 16 && status.installed_version != "1.2.1") {
                    const LangWarningText& t = kLangWarningText[std::clamp(overlay::menu_language(), 0, 12)];
                    const char* ver = status.installed_version.empty() ? "unknown" : status.installed_version.c_str();
                    char body[512];
                    std::snprintf(body, sizeof body, t.body_fmt, ver);
                    auto to_wide = [](const char* s) {
                        int wn = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
                        std::wstring out(size_t(std::max(1, wn)) - 1, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), wn);
                        return out;
                    };
                    MessageBoxW(nullptr, to_wide(body).c_str(), to_wide(t.title).c_str(), MB_OK | MB_ICONWARNING);
                    plat::exit_process(1);
                }
            }
        }
#else
        const char* headless = std::getenv("NX_HEADLESS");
        const fs::path self = self_exe_path();
        if ((!headless || !*headless || std::strcmp(headless, "0") == 0) && !self.empty() &&
            self.filename() == "wonder" && file(self.parent_path() / "wonder-launcher")) {
            const Status status = check_status(find_kit());
            overlay::set_game_version(status.installed_version);
            if (status.ready != Ready::Yes) {
                log("game: cannot start: %s", status.why.c_str());
                std::fprintf(
                    stderr,
                    "SuperWonders - Game setup required\n%s\n\nOpen wonder-launcher, select your game files and "
                    "complete Set up game files.\n",
                    status.why.c_str());
                plat::exit_process(1);
            }
            if (overlay::game_language() >= 16 && status.installed_version != "1.2.1") {
                const LangWarningText& t = kLangWarningText[std::clamp(overlay::menu_language(), 0, 12)];
                const char* ver = status.installed_version.empty() ? "unknown" : status.installed_version.c_str();
                char body[512];
                std::snprintf(body, sizeof body, t.body_fmt, ver);
                std::fprintf(stderr, "%s\n%s\n", t.title, body);
                plat::exit_process(1);
            }
        }
#endif
        return;
    }
    static State state;
    g = &state;
    g->kit = find_kit();
    g->config = load_config();
    g->config_mode = config_mode;
    refresh_status();
    updates_init(config_mode);
    // A regular launch always opens the launcher; RecompLauncher uses --play to bypass it.
    log("launcher: shown (%s; %s; kit %s)", config_mode ? "configuration" : "setup",
        g->status.why.empty() ? "ready" : g->status.why.c_str(), g->kit.script.empty() ? "none" : g->kit.script.c_str());
    if (!config_mode) g->page = g->status.ready == Ready::Yes ? PagePlay : PageFiles;
    g->shown_ns = plat::monotonic_ns();
    overlay::set_screen(&screen);
    discord::set_activity("In Launcher", g->config_mode ? "Configuration" : "Setup");

    // Test hooks (documented here, as Savage Wind's are where they're defined):
    //   NX_LAUNCHER_TOUR=<n>   each page for n frames, then the settings menu over it, then quits (with
    //                          NX_DUMP_PRESENTED=1 NX_DUMP_PREP=<n>: the last frame of each to frames/9NNNNN.ppm)
    //   NX_LAUNCHER_SETUP=1    starts Set up game files at once, with the folders in launcher.ini (then plays, unless
    //                          --config: the configuration screen stays, as after any setup there)
    const int tour = [] {
        const char* e = std::getenv("NX_LAUNCHER_TOUR");
        return e ? std::max(0, std::atoi(e)) : 0;
    }();
    if (const char* e = std::getenv("NX_LAUNCHER_SETUP"); e && *e == '1') start_setup();
    // An update brought a kit that builds the game code differently, and compatible folders are known: build it again
    // at once, as Savage Wind does after an update, instead of asking for the folders again.
    else if (g->status.ready == Ready::Rebuild && g->pair_info.state == PairState::Compatible)
        g->page = PageFiles, start_setup();
    uint64_t frames = 0;
    test_pad_start();  // (NX_TEST_PAD: a scripted controller, launcher/test_pad.h)
    for (;;) {
        const uint64_t t0 = plat::monotonic_ns();
        test_pad_frame(frames);
        plat::PadState pad{};
        plat::read_pad(pad);  // (feeds the overlay's controller navigation: overlay::filter_pad)
        if (tour) {
            const int step = int(frames / uint64_t(tour));
            if (step < kPages - 1) g->page = step;  // (not Quit)
            if (step == kPages - 1 && !overlay::is_open()) g->page = PageSettings, overlay::toggle();
            if (step == kPages) {  // the setup card at 62%, its sheen sweeping
                if (overlay::is_open()) overlay::toggle();
                g->page = PageFiles, g->demo_progress = true, g->job.progress = 0.62f;
                if (!g->job.started_ns) g->job.started_ns = plat::monotonic_ns();
                std::lock_guard lk(g->job.lock);
                g->job.what = "Translating the shaders";
            }
            if (step > kPages && !updates_busy()) plat::exit_process(0);  // (an update finishes first)
        }
        if (!nvn::present_ui_frame()) plat::sleep_ns(16'000'000);
        frames++;
        if (g->quit) {
            log("launcher: quit");
            plat::exit_process(0);
        }
        if (g->play && !g->job.running) {
            refresh_status();
            if (g->status.ready == Ready::Yes) {
                if (overlay::game_language() >= 16 && g->status.installed_version != "1.2.1") {
                    g->play = false;
                    g->open_lang_warning = true;
                    continue;
                }
#if defined(_WIN32)
                std::wstring self(32768, L'\0');
                DWORD n = GetModuleFileNameW(nullptr, self.data(), DWORD(self.size()));
                if (n && n < self.size()) {
                    self.resize(n);
                    if (_wcsicmp(fs::path(self).filename().c_str(), L"wonder-launcher.exe") == 0) {
                        const std::string root_utf8 = plat::data_root();
                        int wn = MultiByteToWideChar(CP_UTF8, 0, root_utf8.c_str(), -1, nullptr, 0);
                        std::wstring root_w(size_t(std::max(1, wn)) - 1, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, root_utf8.c_str(), -1, root_w.data(), wn);
                        SetEnvironmentVariableW(L"NX_DATA_ROOT", root_w.c_str());
                        const std::wstring game = (fs::path(self).parent_path() / L"wonder.exe").wstring();
                        std::wstring command = L"\"" + game + L"\" --play";
                        STARTUPINFOW si{};
                        si.cb = sizeof(si);
                        PROCESS_INFORMATION pi{};
                        if (CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                            CloseHandle(pi.hThread);
                            CloseHandle(pi.hProcess);
                            plat::exit_process(0);
                        }
                        log("launcher: could not start wonder.exe (Windows error %lu)", GetLastError());
                        ui::notify("Could not start wonder.exe. Check that it is in the same folder as the launcher.");
                        g->play = false;
                        continue;
                    }
                }
#else
                const fs::path self = self_exe_path();
                if (!self.empty() && self.filename() == "wonder-launcher") {
                    const fs::path game = self.parent_path() / "wonder";
                    if (file(game)) {
                        setenv("NX_DATA_ROOT", plat::data_root().c_str(), 1);
                        const std::string game_s = str(game);
                        char arg0[] = "wonder";
                        char arg1[] = "--play";
                        char* argv[] = {arg0, arg1, nullptr};
                        execv(game_s.c_str(), argv);
                        log("launcher: could not start wonder (errno %d)", errno);
                        ui::notify("Could not start wonder. Check that it is in the same folder as the launcher.");
                        g->play = false;
                        continue;
                    }
                }
#endif
                break;
            }
            g->play = false;
        }
        const uint64_t spent = plat::monotonic_ns() - t0;
        if (spent < 16'000'000) plat::sleep_ns(int64_t(16'000'000 - spent));
    }
    log("launcher: play (%llu frames shown)", (unsigned long long)frames);
    if (overlay::is_open()) overlay::toggle();
    overlay::set_screen(nullptr);
}

}  // namespace nx::launcher
