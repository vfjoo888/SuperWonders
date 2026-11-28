// The settings menu's tabs (Video, Audio, Controls, Overlay, System), at parity with the Win32 menu
// (pc_menu_win.cpp), whose language settings it shares. The layout follows Savage Wind's in-game
// Settings window (github.com/Revivedx/SavageWindRecomp, runtime/ui/menu.mm menu_render and tabs_body, MIT): a title
// with a close X, tabs, and two-pane pages of option rows with the selected row's description on the right
// (option_rows.cpp). Descriptions are English for now.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "imgui.h"
#include "imgui_internal.h"  // NavCursorVisible

#include <nx/runtime.h>

#include "overlay/option_rows.h"
#include "overlay/overlay_internal.h"
#include "overlay/overlay_ui.h"
#include "nextendo_client.h"
#include "platform.h"
#include "svc_am.h"

namespace nx::nvn {
bool hdr_output();  // nvn_vk.cpp: the swapchain is scRGB (HDR output is active)
}

namespace nx::overlay {

namespace {

struct Labels {
    const char *title, *close, *video, *audio, *controls, *overlay, *window_size, *internal_resolution, *fullscreen,
        *topmost, *volume, *mute, *game_language, *game_language_restart, *control_preset, *standard_desc,
        *nintendo_desc, *restart_note, *frame_cap_label, *system, *memory_limit, *low_vram, *restart_low_vram;
};

constexpr Labels kLabels[] = {
    {"Quick Settings", "F1, Esc or Minus + Plus to close", "Video", "Audio", "Controls", "Overlay",
     "Window / output size", "Internal resolution", "Fullscreen", "Always on top", "Game volume", "Mute audio",
     "Game language", "Restart the game to apply the language.", "Button layout",
     "A: bottom (A / Cross)     B: right (B / Circle)", "A: right (B / Circle)     B: bottom (A / Cross)",
     "Internal resolution applies after restarting the game.", "Frame rate cap", "System", "Memory cleanup threshold", "Low VRAM profile",
     "Restart the game to apply the texture profile."},
    {"Impostazioni", "F1, Esc o Meno + Più per chiudere", "Video", "Audio", "Controlli", "Overlay",
     "Dimensione finestra / output", "Risoluzione interna", "Schermo intero", "Sempre in primo piano",
     "Volume gioco", "Disattiva audio", "Lingua del gioco", "Riavvia il gioco per applicare la lingua.",
     "Disposizione tasti", "A: in basso (A / Croce)     B: a destra (B / Cerchio)",
     "A: a destra (B / Cerchio)     B: in basso (A / Croce)", "La risoluzione interna si applica al riavvio.",
     "Limite FPS", "Sistema", "Soglia pulizia memoria", "Profilo VRAM ridotta",
     "Riavvia il gioco per applicare il profilo texture."},
    {"Einstellungen", "F1, Esc oder Minus + Plus zum Schließen", "Video", "Audio", "Steuerung", "Overlay",
     "Fenster- / Ausgabegröße", "Interne Auflösung", "Vollbild", "Immer im Vordergrund", "Spiel-Lautstärke",
     "Ton stummschalten", "Spielsprache", "Spiel zum Übernehmen neu starten.", "Tastenbelegung",
     "A: unten (A / Kreuz)     B: rechts (B / Kreis)", "A: rechts (B / Kreis)     B: unten (A / Kreuz)",
     "Interne Auflösung wird nach Neustart übernommen.", "FPS-Begrenzung", "System", "Speicherbereinigungsschwelle",
     "Niedriger VRAM-Verbrauch", "Spiel neu starten, um das Texturprofil anzuwenden."},
    {"Paramètres", "F1, Échap ou Moins + Plus pour fermer", "Vidéo", "Audio", "Commandes", "Overlay",
     "Taille fenêtre / sortie", "Résolution interne", "Plein écran", "Toujours au premier plan", "Volume du jeu",
     "Couper le son", "Langue du jeu", "Redémarrez le jeu pour appliquer la langue.", "Disposition des touches",
     "A : bas (A / Croix)     B : droite (B / Cercle)", "A : droite (B / Cercle)     B : bas (A / Croix)",
     "La résolution interne s'applique après redémarrage.", "Limite d’images/s", "Système", "Seuil de nettoyage mémoire",
     "Profil VRAM réduit", "Redémarrez pour appliquer le profil de textures."},
    {"Ajustes rápidos", "F1, Esc o Menos + Más para cerrar", "Vídeo", "Audio", "Controles", "Superposición",
     "Tamaño de ventana / salida", "Resolución interna", "Pantalla completa", "Siempre visible", "Volumen del juego", "Silenciar audio",
     "Idioma del juego", "Reinicia el juego para aplicar el idioma.", "Distribución de botones", "A: abajo (A / Cruz)     B: derecha (B / Círculo)",
     "A: derecha (B / Círculo)     B: abajo (A / Cruz)", "La resolución interna se aplica al reiniciar.", "Límite de FPS", "Sistema", "Umbral de limpieza de memoria", "Perfil de poca VRAM",
     "Reinicia el juego para aplicar el perfil de texturas."},
    {"빠른 설정", "F1, Esc 또는 마이너스 + 플러스로 닫기", "비디오", "오디오", "조작", "오버레이",
     "창 / 출력 크기", "내부 해상도", "전체 화면", "항상 위에 표시", "게임 음량", "음소거",
     "게임 언어", "언어를 적용하려면 게임을 다시 시작하세요.", "버튼 배치", "A: 아래 (A / 십자)     B: 오른쪽 (B / 원)",
     "A: 오른쪽 (B / 원)     B: 아래 (A / 십자)", "내부 해상도는 재시작 후 적용됩니다.", "프레임 제한", "시스템", "메모리 정리 기준", "저 VRAM 프로필",
     "텍스처 프로필을 적용하려면 게임을 다시 시작하세요."},
    {"クイック設定", "F1、Esc またはマイナス + プラスで閉じる", "映像", "音声", "操作", "オーバーレイ",
     "ウィンドウ / 出力サイズ", "内部解像度", "全画面表示", "常に手前に表示", "ゲーム音量", "ミュート",
     "ゲームの言語", "言語の変更にはゲームの再起動が必要です。", "ボタン配置", "A: 下 (A / クロス)     B: 右 (B / サークル)",
     "A: 右 (B / サークル)     B: 下 (A / クロス)", "内部解像度は再起動後に適用されます。", "フレームレート上限", "システム", "メモリ解放のしきい値", "低 VRAM プロファイル",
     "テクスチャ設定の適用には再起動が必要です。"},
    {"الإعدادات السريعة", "اضغط F1 أو Esc أو ناقص + زائد للإغلاق", "الفيديو", "الصوت", "التحكم", "عرض الأداء",
     "حجم النافذة / الإخراج", "الدقة الداخلية", "ملء الشاشة", "دائماً في المقدمة", "مستوى صوت اللعبة", "كتم الصوت",
     "لغة اللعبة", "أعد تشغيل اللعبة لتطبيق اللغة.", "تخطيط الأزرار", "A: أسفل     B: يمين", "A: يمين     B: أسفل",
     "تُطبق الدقة الداخلية بعد إعادة التشغيل.", "حد معدل الإطارات", "النظام", "حد تنظيف الذاكرة", "ملف VRAM منخفض",
     "أعد تشغيل اللعبة لتطبيق ملف الأنسجة."},
    {"Быстрые настройки", "F1, Esc или Минус + Плюс — закрыть", "Видео", "Звук", "Управление", "Оверлей",
     "Размер окна / вывода", "Внутреннее разрешение", "Полный экран", "Поверх других окон", "Громкость игры", "Без звука",
     "Язык игры", "Перезапустите игру, чтобы применить язык.", "Раскладка кнопок", "A: снизу     B: справа", "A: справа     B: снизу",
     "Внутреннее разрешение применится после перезапуска.", "Ограничение FPS", "Система", "Порог очистки памяти", "Профиль низкой VRAM",
     "Перезапустите игру, чтобы применить профиль текстур."},
    {"Hızlı Ayarlar", "Kapatmak için F1, Esc veya Eksi + Artı", "Video", "Ses", "Kontroller", "Performans katmanı",
     "Pencere / çıktı boyutu", "Dahili çözünürlük", "Tam ekran", "Her zaman üstte", "Oyun sesi", "Sesi kapat",
     "Oyun dili", "Dili uygulamak için oyunu yeniden başlatın.", "Düğme düzeni", "A: altta     B: sağda", "A: sağda     B: altta",
     "Dahili çözünürlük yeniden başlatınca uygulanır.", "Kare hızı sınırı", "Sistem", "Bellek temizleme eşiği", "Düşük VRAM profili",
     "Doku profilini uygulamak için oyunu yeniden başlatın."},
    {"Snelle instellingen", "Druk op F1, Esc of Min + Plus om te sluiten", "Beeld", "Audio", "Besturing", "Overlay",
     "Venster- / uitvoergrootte", "Interne resolutie", "Volledig scherm", "Altijd op voorgrond", "Spelvolume", "Geluid dempen",
     "Speltaal", "Start het spel opnieuw om de taal toe te passen.", "Knopindeling", "A: onder     B: rechts", "A: rechts     B: onder",
     "Interne resolutie wordt toegepast na opnieuw starten.", "FPS-limiet", "Systeem", "Geheugenopruimdrempel", "Laag-VRAM-profiel",
     "Start het spel opnieuw om het textuurprofiel toe te passen."},
    {"快速设置", "按 F1、Esc 或减号 + 加号关闭", "视频", "音频", "控制", "性能叠加层",
     "窗口 / 输出大小", "内部解析度", "全屏", "窗口置顶", "游戏音量", "静音",
     "游戏语言", "重新启动游戏以应用语言。", "按键布局", "A：下方     B：右侧", "A：右侧     B：下方",
     "重新启动游戏后应用内部解析度。", "帧率上限", "系统", "内存清理阈值", "低显存配置",
     "重新启动游戏以应用纹理配置。"},
    {"快速設定", "按 F1、Esc 或減號 + 加號關閉", "影片", "音訊", "控制", "效能覆蓋層",
     "視窗 / 輸出大小", "內部解析度", "全螢幕", "視窗置頂", "遊戲音量", "靜音",
     "遊戲語言", "重新啟動遊戲以套用語言。", "按鍵配置", "A：下方     B：右側", "A：右側     B：下方",
     "重新啟動遊戲後套用內部解析度。", "影格率上限", "系統", "記憶體清理閾值", "低顯示記憶體設定",
     "重新啟動遊戲以套用材質設定。"},
};
constexpr const char* kLanguageNames[] = {"English", "Italiano", "Deutsch", "Français", "Español", "한국어", "日本語", "العربية", "Русский", "Türkçe", "Nederlands", "简体中文", "繁體中文"};
constexpr const char* kGameLanguageNames[] = {
    "English", "Italiano", "Deutsch", "Français", "Português (Brasil)", "Español", "한국어", "日本語",
    "Nederlands", "Русский", "Simplified Chinese", "Traditional Chinese",
    "Português", "Español (Latinoamérica)", "Français (Canada)", "English (UK)", "Thai", "Polski",
};
constexpr int kInternalOrder[10] = {0, 9, 8, 7, 1, 2, 3, 4, 5, 6};
constexpr const char* kInternal[13][10] = {
    {"Original / Dynamic", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "Native (screen)"},
    {"Originale / dinamica", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "Nativa (schermo)"},
    {"Original / Dynamisch", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "Nativ (Bildschirm)"},
    {"Originale / dynamique", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "Native (écran)"},
    {"Original / dinámico", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "Nativa (pantalla)"},
    {"원본 / 동적", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "네이티브 (화면)"},
    {"オリジナル / 動的", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "ネイティブ (画面)"},
    {"أصلية / ديناميكية", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "أصلية (الشاشة)"},
    {"Исходное / динамическое", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "Экранное"},
    {"Özgün / Dinamik", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "Doğal (ekran)"},
    {"Origineel / dynamisch", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "Schermresolutie"},
    {"原始 / 动态", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "屏幕原生"},
    {"原始 / 動態", "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "螢幕原生"},
};
constexpr const char* kOverlayLabels[13][6] = {
    {"Metrics overlay", "FPS", "1% lows", "Game speed", "Frametime graph", "Overlay position"},
    {"Overlay metriche", "FPS", "Minimi 1%", "Velocità gioco", "Grafico frametime", "Posizione overlay"},
    {"Metrik-Overlay", "FPS", "1%-Tiefstwerte", "Spieltempo", "Frametime-Diagramm", "Overlay-Position"},
    {"Superposition métriques", "FPS", "1 % les plus bas", "Vitesse du jeu", "Graphique frametime",
     "Position de l’overlay"},
    {"Superposición de métricas", "FPS", "Mínimos del 1 %", "Velocidad del juego", "Gráfico de frametime", "Posición"},
    {"성능 오버레이", "FPS", "1% 하위 FPS", "게임 속도", "프레임 시간 그래프", "오버레이 위치"},
    {"パフォーマンス表示", "FPS", "1% Low", "ゲーム速度", "フレーム時間グラフ", "表示位置"},
    {"عرض الأداء", "FPS", "أدنى 1٪", "سرعة اللعبة", "مخطط زمن الإطار", "موضع العرض"},
    {"Оверлей производительности", "FPS", "1% минимум", "Скорость игры", "График времени кадра", "Положение"},
    {"Performans katmanı", "FPS", "%1 düşük", "Oyun hızı", "Kare süresi grafiği", "Konum"},
    {"Prestatie-overlay", "FPS", "1%-laagste", "Spelsnelheid", "Framet grafiek", "Overlaypositie"},
    {"性能叠加层", "FPS", "1%低帧", "游戏速度", "帧时间图表", "叠加层位置"},
    {"效能覆蓋層", "FPS", "1%低幀", "遊戲速度", "影格時間圖表", "覆蓋層位置"},
};
constexpr const char* kPositions[13][4] = {
    {"Top left", "Top right", "Bottom left", "Bottom right"},
    {"In alto a sinistra", "In alto a destra", "In basso a sinistra", "In basso a destra"},
    {"Oben links", "Oben rechts", "Unten links", "Unten rechts"},
    {"En haut à gauche", "En haut à droite", "En bas à gauche", "En bas à droite"},
    {"Arriba izquierda", "Arriba derecha", "Abajo izquierda", "Abajo derecha"},
    {"Sol üst", "Sağ üst", "Sol alt", "Sağ alt"},
    {"左上", "右上", "左下", "右下"},
    {"أعلى اليسار", "أعلى اليمين", "أسفل اليسار", "أسفل اليمين"},
    {"Сверху слева", "Сверху справа", "Снизу слева", "Снизу справа"},
    {"Sol üst", "Sağ üst", "Sol alt", "Sağ alt"},
    {"Linksboven", "Rechtsboven", "Linksonder", "Rechtsonder"},
    {"左上", "右上", "左下", "右下"},
    {"左上", "右上", "左下", "右下"},
};
constexpr const char* kHeaders[13][2] = {
    {"Keyboard", "Controller"}, {"Tastiera", "Controller"}, {"Tastatur", "Controller"}, {"Clavier", "Manette"},
    {"Teclado", "Mando"}, {"키보드", "컨트롤러"}, {"キーボード", "コントローラー"}, {"لوحة المفاتيح", "وحدة التحكم"},
    {"Клавиатура", "Контроллер"}, {"Klavye", "Kontrolcü"}, {"Toetsenbord", "Controller"},
    {"键盘", "控制器"}, {"鍵盤", "控制器"}};
// Secondary option names and section headings not covered by the main label table.
constexpr const char* kCommon[13][12] = {
    {"Display", "Image", "Frame pacing", "Upscaling", "FSR sharpness", "Anti-aliasing", "Anisotropic filtering", "VSync", "Rumble", "Game", "Quit game", "Menu language"},
    {"Schermo", "Immagine", "Sincronizzazione", "Upscaling", "Nitidezza FSR", "Anti-aliasing", "Filtro anisotropico", "V-Sync", "Vibrazione", "Gioco", "Esci dal gioco", "Lingua menu"},
    {"Anzeige", "Bild", "Frame-Pacing", "Upscaling", "FSR-Schärfe", "Kantenglättung", "Anisotrope Filterung", "VSync", "Vibration", "Spiel", "Spiel beenden", "Menüsprache"},
    {"Affichage", "Image", "Synchronisation", "Mise à l’échelle", "Netteté FSR", "Anticrénelage", "Filtrage anisotrope", "VSync", "Vibration", "Jeu", "Quitter le jeu", "Langue du menu"},
    {"Pantalla", "Imagen", "Ritmo de fotogramas", "Escalado", "Nitidez FSR", "Antialiasing", "Filtrado anisotrópico", "VSync", "Vibración", "Juego", "Salir del juego", "Idioma del menú"},
    {"디스플레이", "화면", "프레임 동기화", "업스케일링", "FSR 선명도", "안티앨리어싱", "비등방성 필터링", "VSync", "진동", "게임", "게임 종료", "메뉴 언어"},
    {"画面", "映像", "フレーム同期", "アップスケーリング", "FSR シャープネス", "アンチエイリアス", "異方性フィルタリング", "VSync", "振動", "ゲーム", "ゲームを終了", "メニュー言語"},
    {"الشاشة", "الصورة", "مزامنة الإطارات", "تحسين الدقة", "حدة FSR", "تنعيم الحواف", "ترشيح متباين الخواص", "مزامنة رأسية", "اهتزاز", "اللعبة", "إنهاء اللعبة", "لغة القائمة"},
    {"Экран", "Изображение", "Синхронизация кадров", "Масштабирование", "Резкость FSR", "Сглаживание", "Анизотропная фильтрация", "VSync", "Вибрация", "Игра", "Выйти из игры", "Язык меню"},
    {"Ekran", "Görüntü", "Kare zamanlaması", "Yükseltme", "FSR keskinliği", "Kenar yumuşatma", "Anizotropik filtreleme", "VSync", "Titreşim", "Oyun", "Oyundan çık", "Menü dili"},
    {"Beeldscherm", "Afbeelding", "Framepacing", "Opschaling", "FSR-scherpte", "Anti-aliasing", "Anisotrope filtering", "VSync", "Trillen", "Spel", "Spel afsluiten", "Menutaal"},
    {"显示", "画面", "帧同步", "升频", "FSR锐化", "抗锯齿", "各向异性过滤", "垂直同步", "震动", "游戏", "退出游戏", "菜单语言"},
    {"顯示", "畫面", "影格同步", "升頻", "FSR銳化", "反鋸齒", "各向異性過濾", "垂直同步", "震動", "遊戲", "結束遊戲", "選單語言"},
};
constexpr const char* kActions[kControlCount] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "L Stick", "R Stick",
    "D-pad Left", "D-pad Up", "D-pad Right", "D-pad Down",
};
constexpr const char* kPhysical[kControlCount] = {
    "A / Cross", "B / Circle", "X / Square", "Y / Triangle", "LB / L1", "RB / R1", "LT / L2", "RT / R2",
    "Start / +", "Back / -", "L3 / LS", "R3 / RS", "D-pad Left", "D-pad Up", "D-pad Right", "D-pad Down",
};
constexpr int kTabs = 8;

namespace desc {
constexpr const char* language = "Language of this menu.";
constexpr const char* window_size = "Size of the game window in pixels. Not available in fullscreen.";
constexpr const char* internal = "The resolution the game renders at. Original lets the game choose (it lowers it under "
                                 "load); higher is sharper and costs more GPU time. Native renders at the size the "
                                 "image is shown, so nothing is stretched. Applies after restarting the game.";
constexpr const char* vsync = "How frames reach the screen. On: waits for the display (no tearing; smooth with "
                              "FreeSync/G-Sync). Fast: the newest frame at each refresh (the default). Adaptive: "
                              "waits unless a frame is late. Off: at once (can tear).";
constexpr const char* frame_cap = "Maximum frame rate. 30, 60, 90, 120, 144 and 240 FPS are available. Higher caps need a "
                                  "display and PC that can sustain them; the game simulation remains at normal speed.";
constexpr const char* fullscreen = "Fill the whole screen. F11 also switches it.";
constexpr const char* topmost = "Keep the game window above other windows. Not available on Wayland.";
constexpr const char* volume = "The game's volume.";
constexpr const char* mute = "Silence the game without changing its volume.";
constexpr const char* game_language = "Select the game's text language. Changes apply after restarting.";
constexpr const char* preset = "Which controller button is A. Xbox / DualSense: A at the bottom. Nintendo: A on the "
                               "right, as on a Switch controller.";
constexpr const char* rumble = "How strongly the controller vibrates when the game asks for it. 0% turns it off.";
constexpr const char* key = "Select, then press the key for this action. Esc cancels.";
constexpr const char* pad = "The controller button for this action: left / right picks another. The layout's default "
                            "clears the change.";
constexpr const char* metrics = "A performance panel in a corner of the screen.";
constexpr const char* fps = "Frames per second over the last 60 frames.";
constexpr const char* low = "The frame rate of the slowest 1% of the last 300 frames: it shows stutter.";
constexpr const char* speed = "How fast the game runs against its own target: 100% is full speed.";
constexpr const char* graph = "The time of each of the last 300 frames, 0 to 50 ms.";
constexpr const char* position = "The corner the panel sits in.";
constexpr const char* memory = "On desktop, passing this threshold asks Vulkan to release unused reusable images and "
                               "keeps the game running. Android uses it as a safety stop. It cannot free textures in use "
                               "and is not a direct GPU VRAM limit.";
constexpr const char* upscaler = "How the game's image is enlarged when it is smaller than the window (a lower internal "
                                 "resolution, or fullscreen above 1080p). FSR 1 (AMD FidelityFX Super Resolution) keeps "
                                 "edges sharp; Bilinear is a plain stretch.";
constexpr const char* antialiasing = "Smooths jagged edges on the final image. FXAA is fast but can soften fine detail; SMAA keeps textures and UI sharper.";

constexpr const char* anisotropy = "Keeps textures sharp on surfaces seen at an angle. Game default uses the game's "
                                   "own setting; higher costs a little GPU time. Applies after restarting the game.";
constexpr const char* sharpness = "FSR 1's sharpening: higher is sharper (more contrast on edges), lower is softer.";
constexpr const char* low_vram = "For graphics cards with little video memory: decoded textures are kept compressed "
                                 "(BC3), at a small cost in quality. Applies after restarting the game.";
constexpr const char* quit = "Close the game. Progress since the game last saved is lost. Select twice to confirm.";
}  // namespace desc


const char* translated_desc(const char* text, int lang) {
    if (lang == 2) {
        if (text == desc::language) return "Sprache dieses Menüs.";
        if (text == desc::window_size) return "Fenstergröße in Pixeln. Im Vollbild nicht verfügbar.";
        if (text == desc::internal) return "Interne Auflösung. Höhere Werte belasten die GPU stärker. Neustart erforderlich.";
        if (text == desc::vsync) return "Synchronisiert Bilder mit dem Bildschirm. Ohne Synchronisation kann Tearing auftreten.";
        if (text == desc::frame_cap) return "Maximale Bildrate. Hohe Werte benötigen einen geeigneten PC und Monitor.";
        if (text == desc::fullscreen) return "Vollbild ein- oder ausschalten. Auch mit F11.";
        if (text == desc::topmost) return "Fenster im Vordergrund halten. Unter Wayland nicht verfügbar.";
        if (text == desc::volume) return "Lautstärke des Spiels.";
        if (text == desc::mute) return "Spiel stummschalten.";
        if (text == desc::game_language) return "Spielsprache auswählen. Neustart erforderlich.";
        if (text == desc::preset) return "Position der A-Taste auf dem Controller.";
        if (text == desc::rumble) return "Vibrationsstärke. 0 % deaktiviert die Vibration.";
        if (text == desc::key) return "Auswählen und Taste drücken. Esc bricht ab.";
        if (text == desc::pad) return "Controller-Taste für diese Aktion auswählen.";
        if (text == desc::metrics) return "Leistungsanzeige einblenden.";
        if (text == desc::fps) return "Bilder pro Sekunde der letzten 60 Bilder.";
        if (text == desc::low) return "Bildrate der langsamsten 1 % der letzten 300 Bilder.";
        if (text == desc::speed) return "Spielgeschwindigkeit: 100 % entspricht normaler Geschwindigkeit.";
        if (text == desc::graph) return "Dauer der letzten 300 Bilder von 0 bis 50 ms.";
        if (text == desc::position) return "Position der Leistungsanzeige.";
        if (text == desc::memory) return "Ab diesem Wert gibt Vulkan ungenutzte Bilder frei. Kein direktes VRAM-Limit.";
        if (text == desc::upscaler) return "Verfahren zur Bildvergrößerung. FSR 1 hält Kanten scharf.";
        if (text == desc::antialiasing) return "Glättet Kanten. FXAA kann feine Details leicht verwischen.";
        if (text == desc::anisotropy) return "Schärft Texturen aus schrägen Blickwinkeln. Neustart erforderlich.";
        if (text == desc::sharpness) return "FSR-1-Schärfe einstellen.";
        if (text == desc::low_vram) return "Komprimiert dekodierte Texturen, um VRAM zu sparen. Neustart erforderlich.";
        if (text == desc::quit) return "Spiel beenden. Ungespeicherter Fortschritt geht verloren. Zweimal auswählen.";
    }
    if (lang == 3) {
        if (text == desc::language) return "Langue de ce menu.";
        if (text == desc::window_size) return "Taille de la fenêtre en pixels. Indisponible en plein écran.";
        if (text == desc::internal) return "Résolution interne. Une valeur élevée sollicite davantage le GPU. Redémarrage nécessaire.";
        if (text == desc::vsync) return "Synchronise les images avec l’écran. Sans synchronisation, des déchirures peuvent apparaître.";
        if (text == desc::frame_cap) return "Nombre maximal d’images par seconde. Une valeur élevée exige un PC et un écran adaptés.";
        if (text == desc::fullscreen) return "Activer le plein écran. F11 permet aussi de le changer.";
        if (text == desc::topmost) return "Garder la fenêtre au premier plan. Indisponible sous Wayland.";
        if (text == desc::volume) return "Volume du jeu.";
        if (text == desc::mute) return "Couper le son sans changer le volume.";
        if (text == desc::game_language) return "Choisir la langue du jeu. Redémarrage nécessaire.";
        if (text == desc::preset) return "Position du bouton A sur la manette.";
        if (text == desc::rumble) return "Intensité des vibrations. À 0 %, elles sont désactivées.";
        if (text == desc::key) return "Sélectionnez puis appuyez sur une touche. Échap annule.";
        if (text == desc::pad) return "Choisir le bouton de manette pour cette action.";
        if (text == desc::metrics) return "Afficher les performances à l’écran.";
        if (text == desc::fps) return "Images par seconde sur les 60 dernières images.";
        if (text == desc::low) return "Fréquence des 1 % d’images les plus lentes parmi les 300 dernières.";
        if (text == desc::speed) return "Vitesse du jeu : 100 % correspond à la vitesse normale.";
        if (text == desc::graph) return "Durée des 300 dernières images, de 0 à 50 ms.";
        if (text == desc::position) return "Position du panneau de performances.";
        if (text == desc::memory) return "Au-delà de ce seuil, Vulkan libère les images inutilisées. Ce n’est pas une limite directe de VRAM.";
        if (text == desc::upscaler) return "Méthode d’agrandissement. FSR 1 préserve la netteté des contours.";
        if (text == desc::antialiasing) return "Lisse les contours. FXAA peut adoucir légèrement les détails.";
        if (text == desc::anisotropy) return "Améliore la netteté des textures vues en biais. Redémarrage nécessaire.";
        if (text == desc::sharpness) return "Régler la netteté de FSR 1.";
        if (text == desc::low_vram) return "Compresse les textures décodées pour économiser la VRAM. Redémarrage nécessaire.";
        if (text == desc::quit) return "Quitter le jeu. La progression non sauvegardée sera perdue. Sélectionnez deux fois.";
    }
    if (lang == 4) {
        if (text == desc::language) return "Idioma de este menú.";
        if (text == desc::window_size) return "Tamaño de la ventana en píxeles. No disponible en pantalla completa.";
        if (text == desc::internal) return "Resolución interna. Los valores altos requieren más potencia gráfica. Requiere reiniciar.";
        if (text == desc::vsync) return "Sincroniza los fotogramas con la pantalla. Sin sincronización puede aparecer tearing.";
        if (text == desc::frame_cap) return "Límite de fotogramas por segundo. Los valores altos requieren un PC y monitor adecuados.";
        if (text == desc::fullscreen) return "Activa la pantalla completa. También se cambia con F11.";
        if (text == desc::topmost) return "Mantiene la ventana encima de otras. No disponible en Wayland.";
        if (text == desc::volume) return "Volumen del juego.";
        if (text == desc::mute) return "Silencia el juego sin cambiar el volumen.";
        if (text == desc::game_language) return "Elige el idioma del juego. Requiere reiniciar.";
        if (text == desc::preset) return "Posición del botón A en el mando.";
        if (text == desc::rumble) return "Intensidad de la vibración. Con 0 % se desactiva.";
        if (text == desc::key) return "Selecciona y pulsa una tecla. Esc cancela.";
        if (text == desc::pad) return "Elige el botón del mando para esta acción.";
        if (text == desc::metrics) return "Muestra un panel de rendimiento.";
        if (text == desc::fps) return "Fotogramas por segundo de los últimos 60 fotogramas.";
        if (text == desc::low) return "Frecuencia del 1 % de los fotogramas más lentos de los últimos 300.";
        if (text == desc::speed) return "Velocidad del juego: 100 % es la velocidad normal.";
        if (text == desc::graph) return "Duración de los últimos 300 fotogramas, de 0 a 50 ms.";
        if (text == desc::position) return "Posición del panel de rendimiento.";
        if (text == desc::memory) return "Al superar este umbral Vulkan libera imágenes sin usar. No es un límite directo de VRAM.";
        if (text == desc::upscaler) return "Método para ampliar la imagen. FSR 1 conserva los bordes nítidos.";
        if (text == desc::antialiasing) return "Suaviza bordes dentados. FXAA puede suavizar detalles finos.";
        if (text == desc::anisotropy) return "Mejora las texturas vistas en ángulo. Requiere reiniciar.";
        if (text == desc::sharpness) return "Ajusta la nitidez de FSR 1.";
        if (text == desc::low_vram) return "Comprime texturas decodificadas para ahorrar VRAM. Requiere reiniciar.";
        if (text == desc::quit) return "Cierra el juego. Se perderá el progreso no guardado. Selecciona dos veces.";
    }
    if (lang == 5) {
        if (text == desc::language) return "이 메뉴의 언어입니다.";
        if (text == desc::window_size) return "창 크기입니다. 전체 화면에서는 사용할 수 없습니다.";
        if (text == desc::internal) return "내부 해상도입니다. 높을수록 GPU 부하가 증가합니다. 재시작 후 적용됩니다.";
        if (text == desc::vsync) return "화면과 프레임을 동기화합니다. 끄면 화면이 찢어질 수 있습니다.";
        if (text == desc::frame_cap) return "최대 프레임 속도입니다. 높은 값에는 충분한 PC와 모니터가 필요합니다.";
        if (text == desc::fullscreen) return "전체 화면으로 전환합니다. F11도 사용할 수 있습니다.";
        if (text == desc::topmost) return "창을 항상 위에 표시합니다. Wayland에서는 사용할 수 없습니다.";
        if (text == desc::volume) return "게임 음량입니다.";
        if (text == desc::mute) return "음량을 바꾸지 않고 소리를 끕니다.";
        if (text == desc::game_language) return "게임 언어를 선택합니다. 재시작 후 적용됩니다.";
        if (text == desc::preset) return "컨트롤러의 A 버튼 위치를 선택합니다.";
        if (text == desc::rumble) return "진동 강도입니다. 0%는 진동을 끕니다.";
        if (text == desc::key) return "선택한 후 키를 누릅니다. Esc로 취소합니다.";
        if (text == desc::pad) return "이 동작에 사용할 컨트롤러 버튼을 선택합니다.";
        if (text == desc::metrics) return "화면에 성능 패널을 표시합니다.";
        if (text == desc::fps) return "최근 60프레임의 초당 프레임 수입니다.";
        if (text == desc::low) return "최근 300프레임 중 가장 느린 1%의 프레임 속도입니다.";
        if (text == desc::speed) return "게임 속도입니다. 100%가 정상 속도입니다.";
        if (text == desc::graph) return "최근 300프레임의 시간입니다. 범위는 0~50ms입니다.";
        if (text == desc::position) return "성능 패널의 위치입니다.";
        if (text == desc::memory) return "이 기준을 넘으면 Vulkan이 사용하지 않는 이미지를 해제합니다. 직접적인 VRAM 제한은 아닙니다.";
        if (text == desc::upscaler) return "이미지 확대 방식을 선택합니다. FSR 1은 가장자리를 선명하게 유지합니다.";
        if (text == desc::antialiasing) return "계단 현상을 줄입니다. FXAA는 세부 묘사를 약간 흐리게 할 수 있습니다.";
        if (text == desc::anisotropy) return "비스듬히 보이는 텍스처를 선명하게 합니다. 재시작 후 적용됩니다.";
        if (text == desc::sharpness) return "FSR 1 선명도를 조절합니다.";
        if (text == desc::low_vram) return "디코딩된 텍스처를 압축해 VRAM을 절약합니다. 재시작 후 적용됩니다.";
        if (text == desc::quit) return "게임을 종료합니다. 저장하지 않은 진행 상황은 사라집니다. 두 번 선택하세요.";
    }
    if (lang == 6) {
        if (text == desc::language) return "このメニューの言語です。";
        if (text == desc::window_size) return "ウィンドウのサイズです。全画面では変更できません。";
        if (text == desc::internal) return "内部解像度です。高いほど GPU の負荷が増えます。再起動後に適用されます。";
        if (text == desc::vsync) return "画面とフレームを同期します。無効にすると画面が乱れることがあります。";
        if (text == desc::frame_cap) return "最大フレームレートです。高い設定には対応する PC とモニターが必要です。";
        if (text == desc::fullscreen) return "全画面表示を切り替えます。F11 でも切り替えられます。";
        if (text == desc::topmost) return "ウィンドウを最前面に表示します。Wayland では利用できません。";
        if (text == desc::volume) return "ゲームの音量です。";
        if (text == desc::mute) return "音量を変えずに音を消します。";
        if (text == desc::game_language) return "ゲームの言語を選択します。再起動後に適用されます。";
        if (text == desc::preset) return "コントローラーの A ボタンの位置です。";
        if (text == desc::rumble) return "振動の強さです。0% で無効になります。";
        if (text == desc::key) return "選択してキーを押します。Esc でキャンセルします。";
        if (text == desc::pad) return "この操作に使うコントローラーボタンを選択します。";
        if (text == desc::metrics) return "画面にパフォーマンスを表示します。";
        if (text == desc::fps) return "直近 60 フレームの毎秒フレーム数です。";
        if (text == desc::low) return "直近 300 フレームのうち最も遅い 1% のフレームレートです。";
        if (text == desc::speed) return "ゲーム速度です。100% が通常速度です。";
        if (text == desc::graph) return "直近 300 フレームの時間を 0〜50ms で表示します。";
        if (text == desc::position) return "パフォーマンス表示の位置です。";
        if (text == desc::memory) return "この値を超えると Vulkan が未使用の画像を解放します。VRAM の直接的な上限ではありません。";
        if (text == desc::upscaler) return "画像の拡大方法です。FSR 1 は輪郭を鮮明に保ちます。";
        if (text == desc::antialiasing) return "輪郭のギザギザを軽減します。FXAA は細部を少しぼかす場合があります。";
        if (text == desc::anisotropy) return "斜めから見たテクスチャを鮮明にします。再起動後に適用されます。";
        if (text == desc::sharpness) return "FSR 1 の鮮明さを調整します。";
        if (text == desc::low_vram) return "デコード済みテクスチャを圧縮して VRAM を節約します。再起動後に適用されます。";
        if (text == desc::quit) return "ゲームを終了します。未保存の進行状況は失われます。2 回選択してください。";
    }
    if (lang == 8) {
        if (text == desc::language) return "Язык этого меню.";
        if (text == desc::window_size) return "Размер окна в пикселях. Недоступно в полноэкранном режиме.";
        if (text == desc::internal) return "Внутреннее разрешение. Высокие значения нагружают GPU. Требуется перезапуск.";
        if (text == desc::vsync) return "Синхронизация кадров с экраном. При отключении возможны разрывы изображения.";
        if (text == desc::frame_cap) return "Максимальная частота кадров. Высоким значениям нужны подходящие ПК и монитор.";
        if (text == desc::fullscreen) return "Переключить полноэкранный режим. Также доступна клавиша F11.";
        if (text == desc::topmost) return "Поверх других окон. Недоступно в Wayland.";
        if (text == desc::volume) return "Громкость игры.";
        if (text == desc::mute) return "Выключить звук без изменения громкости.";
        if (text == desc::game_language) return "Выберите язык игры. Требуется перезапуск.";
        if (text == desc::preset) return "Положение кнопки A на контроллере.";
        if (text == desc::rumble) return "Сила вибрации. 0% отключает вибрацию.";
        if (text == desc::key) return "Выберите и нажмите клавишу. Esc отменяет выбор.";
        if (text == desc::pad) return "Выберите кнопку контроллера для действия.";
        if (text == desc::metrics) return "Показать панель производительности.";
        if (text == desc::fps) return "Кадры в секунду за последние 60 кадров.";
        if (text == desc::low) return "Частота 1% самых медленных из последних 300 кадров.";
        if (text == desc::speed) return "Скорость игры: 100% — нормальная скорость.";
        if (text == desc::graph) return "Время последних 300 кадров от 0 до 50 мс.";
        if (text == desc::position) return "Положение панели производительности.";
        if (text == desc::memory) return "При превышении Vulkan освобождает неиспользуемые изображения. Это не прямой лимит VRAM.";
        if (text == desc::upscaler) return "Способ увеличения изображения. FSR 1 сохраняет чёткость краёв.";
        if (text == desc::antialiasing) return "Сглаживает края. FXAA может слегка размывать детали.";
        if (text == desc::anisotropy) return "Повышает чёткость текстур под углом. Требуется перезапуск.";
        if (text == desc::sharpness) return "Настройка резкости FSR 1.";
        if (text == desc::low_vram) return "Сжимает декодированные текстуры для экономии VRAM. Требуется перезапуск.";
        if (text == desc::quit) return "Выйти из игры. Несохранённый прогресс будет потерян. Выберите дважды.";
    }
    if (lang == 10) {
        if (text == desc::language) return "Taal van dit menu.";
        if (text == desc::window_size) return "Venstergrootte in pixels. Niet beschikbaar in volledig scherm.";
        if (text == desc::internal) return "Interne resolutie. Hogere waarden belasten de GPU meer. Herstart vereist.";
        if (text == desc::vsync) return "Synchroniseert beelden met het scherm. Zonder synchronisatie kan tearing optreden.";
        if (text == desc::frame_cap) return "Maximale beeldsnelheid. Hogere waarden vereisen een geschikte pc en monitor.";
        if (text == desc::fullscreen) return "Schakelt volledig scherm in. Gebruik ook F11.";
        if (text == desc::topmost) return "Houdt het venster boven andere vensters. Niet beschikbaar op Wayland.";
        if (text == desc::volume) return "Volume van het spel.";
        if (text == desc::mute) return "Dempt het spel zonder het volume te wijzigen.";
        if (text == desc::game_language) return "Kies de speltaal. Herstart vereist.";
        if (text == desc::preset) return "Positie van de A-knop op de controller.";
        if (text == desc::rumble) return "Trilsterkte. 0% schakelt trillingen uit.";
        if (text == desc::key) return "Selecteer en druk op een toets. Esc annuleert.";
        if (text == desc::pad) return "Kies de controllerknop voor deze actie.";
        if (text == desc::metrics) return "Toont een prestatiepaneel.";
        if (text == desc::fps) return "Beelden per seconde over de laatste 60 beelden.";
        if (text == desc::low) return "Beeldsnelheid van de traagste 1% van de laatste 300 beelden.";
        if (text == desc::speed) return "Spelsnelheid: 100% is normale snelheid.";
        if (text == desc::graph) return "Tijd van de laatste 300 beelden, van 0 tot 50 ms.";
        if (text == desc::position) return "Positie van het prestatiepaneel.";
        if (text == desc::memory) return "Boven deze grens geeft Vulkan ongebruikte afbeeldingen vrij. Dit is geen directe VRAM-limiet.";
        if (text == desc::upscaler) return "Methode om de afbeelding te vergroten. FSR 1 houdt randen scherp.";
        if (text == desc::antialiasing) return "Maakt randen glad. FXAA kan details iets verzachten.";
        if (text == desc::anisotropy) return "Maakt schuin bekeken texturen scherper. Herstart vereist.";
        if (text == desc::sharpness) return "Past de scherpte van FSR 1 aan.";
        if (text == desc::low_vram) return "Comprimeert gedecodeerde texturen om VRAM te besparen. Herstart vereist.";
        if (text == desc::quit) return "Sluit het spel. Niet-opgeslagen voortgang gaat verloren. Selecteer twee keer.";
    }
    if (lang == 11) {
        if (text == desc::language) return "此菜单的语言。";
        if (text == desc::window_size) return "窗口的像素大小。全屏时不可用。";
        if (text == desc::internal) return "内部渲染分辨率。较高数值需要更多显卡性能。重启后生效。";
        if (text == desc::vsync) return "将帧与屏幕同步。关闭后可能出现画面撕裂。";
        if (text == desc::frame_cap) return "最大帧率。较高数值需要性能足够的电脑和显示器。";
        if (text == desc::fullscreen) return "切换全屏。也可以按 F11。";
        if (text == desc::topmost) return "窗口置顶。Wayland 下不可用。";
        if (text == desc::volume) return "游戏音量。";
        if (text == desc::mute) return "不改变音量即可静音。";
        if (text == desc::game_language) return "选择游戏语言。重启后生效。";
        if (text == desc::preset) return "手柄 A 键的位置。";
        if (text == desc::rumble) return "振动强度。0% 表示关闭。";
        if (text == desc::key) return "选择后按下按键。Esc 取消。";
        if (text == desc::pad) return "选择此操作的手柄按键。";
        if (text == desc::metrics) return "显示性能面板。";
        if (text == desc::fps) return "最近 60 帧的每秒帧数。";
        if (text == desc::low) return "最近 300 帧中最慢 1% 的帧率。";
        if (text == desc::speed) return "游戏速度：100% 为正常速度。";
        if (text == desc::graph) return "最近 300 帧的耗时，范围为 0 至 50 毫秒。";
        if (text == desc::position) return "性能面板的位置。";
        if (text == desc::memory) return "超过阈值时 Vulkan 会释放未使用的图像。这不是直接的显存上限。";
        if (text == desc::upscaler) return "图像放大方式。FSR 1 可保持边缘清晰。";
        if (text == desc::antialiasing) return "平滑锯齿。FXAA 可能稍微模糊细节。";
        if (text == desc::anisotropy) return "提高斜向纹理清晰度。重启后生效。";
        if (text == desc::sharpness) return "调整 FSR 1 锐度。";
        if (text == desc::low_vram) return "压缩解码后的纹理以节省显存。重启后生效。";
        if (text == desc::quit) return "退出游戏。未保存的进度将丢失。请选择两次确认。";
    }
    if (lang == 12) {
        if (text == desc::language) return "此選單的語言。";
        if (text == desc::window_size) return "視窗的像素大小。全螢幕時無法使用。";
        if (text == desc::internal) return "內部算繪解析度。較高數值需要更多顯示卡效能。重新啟動後生效。";
        if (text == desc::vsync) return "將影格與螢幕同步。關閉後可能出現畫面撕裂。";
        if (text == desc::frame_cap) return "最大影格率。較高數值需要效能足夠的電腦與螢幕。";
        if (text == desc::fullscreen) return "切換全螢幕。也可以按 F11。";
        if (text == desc::topmost) return "視窗置頂。Wayland 下無法使用。";
        if (text == desc::volume) return "遊戲音量。";
        if (text == desc::mute) return "不改變音量即可靜音。";
        if (text == desc::game_language) return "選擇遊戲語言。重新啟動後生效。";
        if (text == desc::preset) return "控制器 A 鍵的位置。";
        if (text == desc::rumble) return "震動強度。0% 表示關閉。";
        if (text == desc::key) return "選擇後按下按鍵。Esc 取消。";
        if (text == desc::pad) return "選擇此操作的控制器按鍵。";
        if (text == desc::metrics) return "顯示效能面板。";
        if (text == desc::fps) return "最近 60 個影格的每秒影格數。";
        if (text == desc::low) return "最近 300 個影格中最慢 1% 的影格率。";
        if (text == desc::speed) return "遊戲速度：100% 為正常速度。";
        if (text == desc::graph) return "最近 300 個影格的耗時，範圍為 0 至 50 毫秒。";
        if (text == desc::position) return "效能面板的位置。";
        if (text == desc::memory) return "超過門檻時 Vulkan 會釋放未使用的影像。這不是直接的顯示記憶體上限。";
        if (text == desc::upscaler) return "影像放大方式。FSR 1 可保持邊緣清晰。";
        if (text == desc::antialiasing) return "平滑鋸齒。FXAA 可能稍微模糊細節。";
        if (text == desc::anisotropy) return "提高斜向材質清晰度。重新啟動後生效。";
        if (text == desc::sharpness) return "調整 FSR 1 銳利度。";
        if (text == desc::low_vram) return "壓縮解碼後的材質以節省顯示記憶體。重新啟動後生效。";
        if (text == desc::quit) return "結束遊戲。未儲存的進度將遺失。請選擇兩次確認。";
    }
    if (lang == 7) {
        if (text == desc::language) return "لغة هذه القائمة.";
        if (text == desc::window_size) return "حجم النافذة بالبكسل. غير متاح في وضع ملء الشاشة.";
        if (text == desc::internal) return "دقة العرض الداخلية. القيم الأعلى تحتاج إلى قوة رسومية أكبر. تسري بعد إعادة التشغيل.";
        if (text == desc::vsync) return "مزامنة الإطارات مع الشاشة. قد يظهر تمزق الصورة عند إيقافها.";
        if (text == desc::frame_cap) return "الحد الأقصى للإطارات في الثانية. القيم الأعلى تحتاج إلى جهاز وشاشة مناسبين.";
        if (text == desc::fullscreen) return "تبديل ملء الشاشة. يمكن استخدام F11 أيضاً.";
        if (text == desc::topmost) return "إبقاء النافذة في المقدمة. غير متاح على Wayland.";
        if (text == desc::volume) return "مستوى صوت اللعبة.";
        if (text == desc::mute) return "كتم صوت اللعبة دون تغيير مستوى الصوت.";
        if (text == desc::game_language) return "اختر لغة اللعبة. تسري بعد إعادة التشغيل.";
        if (text == desc::preset) return "موضع زر A على وحدة التحكم.";
        if (text == desc::rumble) return "شدة اهتزاز وحدة التحكم. 0% يعطل الاهتزاز.";
        if (text == desc::key) return "اختر ثم اضغط مفتاحاً. اضغط Esc للإلغاء.";
        if (text == desc::pad) return "اختر زر وحدة التحكم لهذا الإجراء.";
        if (text == desc::metrics) return "عرض لوحة الأداء على الشاشة.";
        if (text == desc::fps) return "الإطارات في الثانية خلال آخر 60 إطاراً.";
        if (text == desc::low) return "معدل أبطأ 1% من آخر 300 إطار.";
        if (text == desc::speed) return "سرعة اللعبة: 100% هي السرعة الطبيعية.";
        if (text == desc::graph) return "مدة آخر 300 إطار من 0 إلى 50 مللي ثانية.";
        if (text == desc::position) return "موضع لوحة الأداء.";
        if (text == desc::memory) return "عند تجاوز الحد يحرر Vulkan الصور غير المستخدمة. هذا ليس حداً مباشراً لذاكرة الفيديو.";
        if (text == desc::upscaler) return "طريقة تكبير الصورة. يحافظ FSR 1 على وضوح الحواف.";
        if (text == desc::antialiasing) return "ينعم الحواف المتعرجة. قد يقلل FXAA وضوح التفاصيل قليلاً.";
        if (text == desc::anisotropy) return "يحسن وضوح الخامات المائلة. يسري بعد إعادة التشغيل.";
        if (text == desc::sharpness) return "ضبط حدة FSR 1.";
        if (text == desc::low_vram) return "ضغط الخامات المفككة لتوفير ذاكرة الفيديو. يسري بعد إعادة التشغيل.";
        if (text == desc::quit) return "إغلاق اللعبة. سيضيع التقدم غير المحفوظ. اختر مرتين للتأكيد.";
    }
    if (lang == 9) {
        if (text == desc::language) return "Bu menünün dili.";
        if (text == desc::window_size) return "Pencere boyutu piksel cinsindendir. Tam ekranda kullanılamaz.";
        if (text == desc::internal) return "Dahili çözünürlük. Daha yüksek değerler GPU yükünü artırır. Yeniden başlatınca uygulanır.";
        if (text == desc::vsync) return "Kareleri ekranla eşzamanlar. Kapalıyken görüntü yırtılması oluşabilir.";
        if (text == desc::frame_cap) return "En yüksek kare hızı. Yüksek değerler uygun bir bilgisayar ve ekran gerektirir.";
        if (text == desc::fullscreen) return "Tam ekranı değiştirir. F11 de kullanılabilir.";
        if (text == desc::topmost) return "Pencereyi diğerlerinin üstünde tutar. Wayland üzerinde kullanılamaz.";
        if (text == desc::volume) return "Oyunun ses düzeyi.";
        if (text == desc::mute) return "Ses düzeyini değiştirmeden oyunu sessize alır.";
        if (text == desc::game_language) return "Oyun dilini seçin. Yeniden başlatınca uygulanır.";
        if (text == desc::preset) return "Kontrolcüdeki A düğmesinin konumu.";
        if (text == desc::rumble) return "Titreşim gücü. %0 titreşimi kapatır.";
        if (text == desc::key) return "Seçip bir tuşa basın. Esc iptal eder.";
        if (text == desc::pad) return "Bu işlem için kontrolcü düğmesini seçin.";
        if (text == desc::metrics) return "Ekranda performans panelini gösterir.";
        if (text == desc::fps) return "Son 60 karede saniye başına kare sayısı.";
        if (text == desc::low) return "Son 300 karenin en yavaş %1 bölümünün kare hızı.";
        if (text == desc::speed) return "Oyun hızı: %100 normal hızdır.";
        if (text == desc::graph) return "Son 300 karenin süresi, 0 ile 50 ms arasında.";
        if (text == desc::position) return "Performans panelinin konumu.";
        if (text == desc::memory) return "Bu eşik aşılınca Vulkan kullanılmayan görüntüleri serbest bırakır. Doğrudan VRAM sınırı değildir.";
        if (text == desc::upscaler) return "Görüntüyü büyütme yöntemi. FSR 1 kenarları keskin tutar.";
        if (text == desc::antialiasing) return "Tırtıklı kenarları yumuşatır. FXAA ince ayrıntıları biraz yumuşatabilir.";
        if (text == desc::anisotropy) return "Açıyla görülen dokuları keskinleştirir. Yeniden başlatınca uygulanır.";
        if (text == desc::sharpness) return "FSR 1 keskinliğini ayarlar.";
        if (text == desc::low_vram) return "VRAM tasarrufu için çözümlenmiş dokuları sıkıştırır. Yeniden başlatınca uygulanır.";
        if (text == desc::quit) return "Oyunu kapatır. Kaydedilmemiş ilerleme kaybolur. Onaylamak için iki kez seçin.";
    }
    if (lang != 1) return text;
    if (text == desc::language) return "Lingua di questo menu.";
    if (text == desc::window_size) return "Dimensioni della finestra in pixel. Non disponibile a schermo intero.";
    if (text == desc::internal) return "Risoluzione del gioco. Originale lascia scegliere al gioco; valori più alti richiedono più potenza grafica. Si applica al riavvio.";
    if (text == desc::vsync) return "Sincronizza i fotogrammi con lo schermo. Disattivandola possono comparire interruzioni nell’immagine.";
    if (text == desc::frame_cap) return "Limite massimo di fotogrammi al secondo. Valori più alti richiedono un PC e uno schermo adeguati.";
    if (text == desc::fullscreen) return "Mostra il gioco a schermo intero. Puoi usare anche F11.";
    if (text == desc::topmost) return "Mantiene la finestra sopra le altre. Non disponibile su Wayland.";
    if (text == desc::volume) return "Regola il volume del gioco.";
    if (text == desc::mute) return "Disattiva il suono senza cambiare il volume.";
    if (text == desc::game_language) return "Scegli la lingua del gioco. La modifica si applica al riavvio.";
    if (text == desc::preset) return "Scegli la posizione del pulsante A sul controller.";
    if (text == desc::rumble) return "Regola la vibrazione del controller. A 0% è disattivata.";
    if (text == desc::key) return "Seleziona e premi il tasto desiderato. Esc annulla.";
    if (text == desc::pad) return "Scegli il pulsante del controller per questa azione.";
    if (text == desc::metrics) return "Mostra un pannello delle prestazioni sullo schermo.";
    if (text == desc::fps) return "Fotogrammi al secondo negli ultimi 60 fotogrammi.";
    if (text == desc::low) return "Frequenza dell’1% dei fotogrammi più lenti tra gli ultimi 300.";
    if (text == desc::speed) return "Velocità del gioco rispetto alla velocità prevista: 100% è normale.";
    if (text == desc::graph) return "Durata degli ultimi 300 fotogrammi, da 0 a 50 ms.";
    if (text == desc::position) return "Scegli l’angolo in cui mostrare il pannello.";
    if (text == desc::memory) return "Soglia oltre la quale Vulkan libera le immagini riutilizzabili non in uso. Non è un limite diretto della VRAM.";
    if (text == desc::upscaler) return "Scegli come ingrandire l’immagine. FSR 1 mantiene i contorni più nitidi.";
    if (text == desc::antialiasing) return "Riduce le scalettature. FXAA è veloce ma può ammorbidire i dettagli; SMAA mantiene texture e interfaccia più nitide.";
    if (text == desc::anisotropy) return "Migliora la nitidezza delle texture viste di sbieco. Si applica al riavvio.";
    if (text == desc::sharpness) return "Regola la nitidezza di FSR 1.";
    if (text == desc::low_vram) return "Mantiene compresse le texture decodificate per ridurre la memoria video, con una lieve perdita di qualità. Si applica al riavvio.";
    if (text == desc::quit) return "Chiude il gioco. I progressi non salvati andranno persi. Seleziona due volte per confermare.";
    return text;
}

const Settings kDefaults{};

struct ExtraUiText {
    const char *reset_sec, *reset_tab_fmt, *reset_tab_desc, *reset_tab_note,
               *reset_all, *reset_all_confirm, *reset_all_desc, *reset_all_note,
               *style_color, *style_color_desc;
};
constexpr ExtraUiText kExtraUiText[13] = {
    {"Reset", "Reset %s", "Restore default settings for this tab.", "Tab settings reset to defaults.",
     "Reset all settings", "Press again to confirm", "Reset every settings tab to factory defaults.",
     "All settings reset to defaults.", "Style color", "Accent color theme for the launcher and in-game settings."},
    {"Ripristino", "Ripristina %s", "Ripristina i valori predefiniti di questa scheda.",
     "Impostazioni della scheda ripristinate.", "Ripristina tutte le impostazioni",
     "Premi di nuovo per confermare", "Ripristina tutte le schede ai valori predefiniti.",
     "Tutte le impostazioni sono state ripristinate.", "Colore stile",
     "Tema colori dell'interfaccia per launcher e impostazioni."},
    {"Zurücksetzen", "%s zurücksetzen", "Standardwerte für diesen Tab wiederherstellen.",
     "Tab-Einstellungen zurückgesetzt.", "Alle Einstellungen zurücksetzen",
     "Zum Bestätigen erneut wählen", "Alle Tabs auf die Standardwerte zurücksetzen.",
     "Alle Einstellungen zurückgesetzt.", "Stilfarbe",
     "Akzentfarbe für den Launcher und das Menü im Spiel."},
    {"Réinitialiser", "Réinitialiser %s", "Restaurer les paramètres par défaut de cet onglet.",
     "Paramètres de l'onglet réinitialisés.", "Tout réinitialiser",
     "Sélectionner encore pour confirmer", "Réinitialiser tous les onglets par défaut.",
     "Tous les paramètres ont été réinitialisés.", "Couleur du thème",
     "Couleur d'accentuation du lanceur et du menu en jeu."},
    {"Restablecer", "Restablecer %s", "Restaurar los valores predeterminados de esta pestaña.",
     "Ajustes de la pestaña restablecidos.", "Restablecer todos los ajustes",
     "Pulsa de nuevo para confirmar", "Restablecer todas las pestañas a los valores de fábrica.",
     "Todos los ajustes restablecidos.", "Color de estilo",
     "Color de énfasis para el lanzador y los ajustes del juego."},
    {"초기화", "%s 초기화", "이 탭의 설정을 기본값으로 되돌립니다.",
     "이 탭의 설정이 초기화되었습니다.", "모든 설정 초기화",
     "다시 눌러 확인", "모든 설정 탭을 기본값으로 되돌립니다.",
     "모든 설정이 초기화되었습니다.", "스타일 색상",
     "런처와 게임 내 설정의 강조 색상입니다."},
    {"リセット", "%sをリセット", "このタブの設定を初期値に戻します。",
     "このタブの設定をリセットしました。", "すべての設定をリセット",
     "もう一度押して確認", "すべての設定タブを初期値に戻します。",
     "すべての設定をリセットしました。", "スタイルカラー",
     "ランチャーとゲーム内設定のアクセントカラーです。"},
    {"إعادة تعيين", "إعادة تعيين %s", "استعادة الإعدادات الافتراضية لهذا التبويب.",
     "تمت إعادة تعيين إعدادات التبويب.", "إعادة تعيين كل الإعدادات",
     "اضغط مرة أخرى للتأكيد", "إعادة تعيين جميع التبويبات إلى الإعدادات الافتراضية.",
     "تمت إعادة تعيين كل الإعدادات.", "لون النمط",
     "لون التمييز للمشغّل وقائمة الإعدادات داخل اللعبة."},
    {"Сброс", "Сбросить: %s", "Восстановить настройки этой вкладки по умолчанию.",
     "Настройки вкладки сброшены.", "Сбросить все настройки",
     "Нажмите ещё раз для подтверждения", "Сбросить все вкладки к значениям по умолчанию.",
     "Все настройки сброшены.", "Цвет темы",
     "Акцентный цвет интерфейса лаунчера и меню настроек."},
    {"Sıfırla", "%s sıfırla", "Bu sekmenin ayarlarını varsayılana döndürür.",
     "Sekme ayarları sıfırlandı.", "Tüm ayarları sıfırla",
     "Onaylamak için tekrar seçin", "Tüm ayar sekmelerini varsayılan değerlere döndürür.",
     "Tüm ayarlar sıfırlandı.", "Stil rengi",
     "Başlatıcı ve oyun içi ayarlar için vurgu rengi."},
    {"Herstellen", "%s herstellen", "Standaardinstellingen voor dit tabblad herstellen.",
     "Tabbladinstellingen hersteld.", "Alle instellingen herstellen",
     "Nogmaals drukken om te bevestigen", "Alle tabbladen terugzetten naar standaardwaarden.",
     "Alle instellingen hersteld.", "Stijlkleur",
     "Accentkleur voor de launcher en de instellingen in het spel."},
    {"重置", "重置%s", "将此选项卡的设置恢复为默认值。",
     "已重置此选项卡设置。", "重置所有设置",
     "再次选择以确认", "将所有设置选项卡恢复为出厂默认值。",
     "所有设置已重置。", "界面主题色",
     "启动器和游戏内设置的主题配色。"},
    {"重設", "重設%s", "將此分頁的設定恢復為預設值。",
     "已重設此分頁設定。", "重設所有設定",
     "再次選擇以確認", "將所有設定分頁恢復為出廠預設值。",
     "所有設定已重設。", "介面主題色",
     "啟動器與遊戲內設定的主題配色。"},
};
static_assert(std::size(kExtraUiText) == std::size(kLabels));

#if !defined(__ANDROID__) && !defined(__APPLE__)
// HDR output (docs/HDR.md): section, toggle label, its description, restart note, "not active" note, then label and
// description of Peak brightness, Paper white, Highlights and Highlight start.
constexpr const char* kHdrText[13][13] = {
    {"HDR", "HDR output",
     "Sends the picture in HDR, so the highlights the game clips to white (sun, glows, sparkles) shine brighter while "
     "the rest looks as in SDR. Needs Windows HDR turned on for this display. Applies after restarting the game.",
     "Restart the game to switch HDR output.",
     "HDR is not active now: Windows HDR is off for this display (or not supported), so the game shows SDR.",
     "Peak brightness",
     "The brightest your display can show, in nits (LG OLEDs: about 1000; the Windows HDR Calibration app measures "
     "it). Highlights roll off smoothly into it.",
     "Paper white",
     "How bright the SDR part of the picture, menus and text are, in nits. 203 is the HDR reference; raise it in a "
     "bright room.",
     "Highlights",
     "How far past white the highlights go. 0% clips them as in SDR; higher makes them brighter. Midtones and the UI "
     "never change.",
     "Highlight start",
     "Where the highlights begin, in percent of the scene brightness the game shows as white. 100%: they grow smoothly "
     "out of white. Lower: they rise faster; higher: more of them stays plain white, as in SDR."},
    {"HDR", "Uscita HDR",
     "Invia l’immagine in HDR: le luci che il gioco taglia al bianco (sole, bagliori, scintille) risaltano di più, il "
     "resto resta come in SDR. Richiede l’HDR di Windows attivo per questo schermo. Si applica al riavvio.",
     "Riavvia il gioco per cambiare l’uscita HDR.",
     "L’HDR non è attivo: l’HDR di Windows è disattivato per questo schermo (o non supportato), il gioco è in SDR.",
     "Luminosità di picco",
     "La luminosità massima dello schermo, in nit (OLED LG: circa 1000; l’app Calibrazione HDR di Windows la misura). "
     "Le luci vi si avvicinano dolcemente.",
     "Bianco carta",
     "Luminosità della parte SDR dell’immagine, dei menu e del testo, in nit. 203 è il riferimento HDR; aumentalo in "
     "una stanza luminosa.",
     "Luci",
     "Quanto le luci vanno oltre il bianco. 0% le taglia come in SDR; valori più alti le rendono più brillanti. Toni "
     "medi e interfaccia non cambiano mai.",
     "Inizio luci",
     "Dove iniziano le luci, in percentuale della luminosità che il gioco mostra come bianco. 100%: crescono dolcemente "
     "dal bianco. Meno: salgono più in fretta; di più: una parte maggiore resta bianca come in SDR."},
    {"HDR", "HDR-Ausgabe",
     "Gibt das Bild in HDR aus: Lichter, die das Spiel weiß abschneidet (Sonne, Leuchten, Funkeln), strahlen heller, der "
     "Rest bleibt wie in SDR. Windows-HDR muss für diesen Bildschirm an sein. Wird nach Neustart übernommen.",
     "Spiel neu starten, um die HDR-Ausgabe umzuschalten.",
     "HDR ist nicht aktiv: Windows-HDR ist für diesen Bildschirm aus (oder nicht unterstützt), das Spiel zeigt SDR.",
     "Spitzenhelligkeit",
     "Die höchste Helligkeit des Bildschirms in Nits (LG-OLEDs: etwa 1000; die Windows-HDR-Kalibrierung misst sie). "
     "Lichter laufen weich darauf zu.",
     "Papierweiß",
     "Helligkeit des SDR-Bildanteils, der Menüs und Texte in Nits. 203 ist der HDR-Referenzwert; in hellen Räumen "
     "erhöhen.",
     "Lichter",
     "Wie weit Lichter über Weiß hinausgehen. 0 % schneidet sie wie in SDR ab; höher macht sie heller. Mitteltöne und "
     "Oberfläche ändern sich nie.",
     "Lichter-Beginn",
     "Wo die Lichter beginnen, in Prozent der Szenenhelligkeit, die das Spiel als Weiß zeigt. 100 %: sie wachsen weich "
     "aus Weiß. Weniger: sie steigen schneller; mehr: ein größerer Teil bleibt weiß wie in SDR."},
    {"HDR", "Sortie HDR",
     "Envoie l’image en HDR : les lumières que le jeu écrête au blanc (soleil, halos, étincelles) brillent davantage, "
     "le reste reste comme en SDR. Nécessite le HDR de Windows activé pour cet écran. S’applique après redémarrage.",
     "Redémarrez le jeu pour changer la sortie HDR.",
     "Le HDR n’est pas actif : le HDR de Windows est désactivé pour cet écran (ou non pris en charge), le jeu est en SDR.",
     "Luminosité de crête",
     "La luminosité maximale de l’écran, en nits (OLED LG : environ 1000 ; l’app Étalonnage HDR de Windows la mesure). "
     "Les lumières s’en approchent en douceur.",
     "Blanc papier",
     "Luminosité de la partie SDR de l’image, des menus et du texte, en nits. 203 est la référence HDR ; augmentez-la "
     "dans une pièce claire.",
     "Hautes lumières",
     "Jusqu’où les lumières dépassent le blanc. 0 % les écrête comme en SDR ; plus haut les rend plus brillantes. Les "
     "tons moyens et l’interface ne changent jamais.",
     "Début des lumières",
     "Où commencent les lumières, en pourcentage de la luminosité que le jeu affiche en blanc. 100 % : elles naissent "
     "en douceur du blanc. Moins : elles montent plus vite ; plus : une plus grande part reste blanche comme en SDR."},
    {"HDR", "Salida HDR",
     "Envía la imagen en HDR: las luces que el juego recorta a blanco (sol, brillos, destellos) brillan más y el resto "
     "se ve como en SDR. Requiere el HDR de Windows activado para esta pantalla. Se aplica al reiniciar.",
     "Reinicia el juego para cambiar la salida HDR.",
     "El HDR no está activo: el HDR de Windows está desactivado en esta pantalla (o no es compatible) y el juego va en SDR.",
     "Brillo máximo",
     "El brillo máximo de la pantalla, en nits (OLED de LG: unos 1000; la app Calibración HDR de Windows lo mide). Las "
     "luces se acercan a él con suavidad.",
     "Blanco papel",
     "Brillo de la parte SDR de la imagen, los menús y el texto, en nits. 203 es la referencia HDR; súbelo en una sala "
     "luminosa.",
     "Luces",
     "Cuánto pasan las luces del blanco. 0 % las recorta como en SDR; más alto las hace más brillantes. Los tonos "
     "medios y la interfaz nunca cambian.",
     "Inicio de luces",
     "Dónde empiezan las luces, en porcentaje del brillo que el juego muestra como blanco. 100 %: crecen suavemente "
     "desde el blanco. Menos: suben más rápido; más: una parte mayor queda blanca como en SDR."},
    {"HDR", "HDR 출력",
     "화면을 HDR로 출력합니다. 게임이 흰색으로 잘라내던 하이라이트(태양, 빛, 반짝임)가 더 밝게 빛나고 나머지는 SDR과 "
     "같습니다. 이 디스플레이에서 Windows HDR이 켜져 있어야 합니다. 재시작 후 적용됩니다.",
     "HDR 출력을 바꾸려면 게임을 다시 시작하세요.",
     "HDR이 활성화되지 않았습니다. 이 디스플레이의 Windows HDR이 꺼져 있거나 지원되지 않아 SDR로 표시됩니다.",
     "최대 밝기",
     "디스플레이가 낼 수 있는 최대 밝기(니트)입니다 (LG OLED: 약 1000, Windows HDR 보정 앱으로 측정). 하이라이트가 "
     "부드럽게 이 값에 수렴합니다.",
     "종이 흰색",
     "SDR 화면, 메뉴, 텍스트의 밝기(니트)입니다. 203이 HDR 기준값이며 밝은 방에서는 높이세요.",
     "하이라이트",
     "하이라이트가 흰색을 얼마나 넘는지입니다. 0%는 SDR처럼 잘라내고, 높을수록 밝아집니다. 중간 톤과 UI는 바뀌지 않습니다.",
     "하이라이트 시작",
     "하이라이트가 시작되는 지점으로, 게임이 흰색으로 보여 주는 장면 밝기의 백분율입니다. 100%: 흰색에서 부드럽게 "
     "올라갑니다. 낮게: 더 빨리 밝아지고, 높게: SDR처럼 흰색으로 남는 부분이 늘어납니다."},
    {"HDR", "HDR 出力",
     "映像を HDR で出力します。ゲームが白に切り詰めていたハイライト（太陽、光、きらめき）がより明るく輝き、それ以外は "
     "SDR と同じです。このディスプレイで Windows の HDR をオンにする必要があります。再起動後に適用されます。",
     "HDR 出力を切り替えるにはゲームを再起動してください。",
     "HDR は有効になっていません。このディスプレイの Windows HDR がオフ（または非対応）のため、SDR で表示しています。",
     "ピーク輝度",
     "ディスプレイの最大輝度（ニト）です（LG の有機 EL: 約 1000。Windows HDR 調整アプリで測定できます）。ハイライトは"
     "なめらかにここへ収まります。",
     "ペーパーホワイト",
     "SDR 部分の映像、メニュー、文字の明るさ（ニト）です。203 が HDR の基準値です。明るい部屋では上げてください。",
     "ハイライト",
     "ハイライトが白をどこまで超えるかです。0% は SDR と同じく切り詰め、高いほど明るくなります。中間調と UI は変わりません。",
     "ハイライト開始",
     "ハイライトが始まる位置で、ゲームが白として表示するシーンの明るさに対する割合です。100%: 白からなめらかに伸びます。"
     "低い: より早く明るくなり、高い: SDR のように白のまま残る部分が増えます。"},
    {"HDR", "إخراج HDR",
     "يرسل الصورة بتقنية HDR، فتسطع الإضاءات التي تقصها اللعبة إلى الأبيض (الشمس، التوهج، البريق) أكثر ويبقى الباقي كما في "
     "SDR. يتطلب تشغيل HDR في Windows لهذه الشاشة. يسري بعد إعادة التشغيل.",
     "أعد تشغيل اللعبة لتبديل إخراج HDR.",
     "HDR غير نشط: HDR في Windows متوقف لهذه الشاشة (أو غير مدعوم)، لذا تعرض اللعبة SDR.",
     "أقصى سطوع",
     "أعلى سطوع يمكن لشاشتك عرضه بوحدة النيت (شاشات LG OLED: نحو 1000؛ يقيسه تطبيق معايرة HDR في Windows). تقترب "
     "الإضاءات منه بسلاسة.",
     "أبيض الورق",
     "سطوع الجزء SDR من الصورة والقوائم والنصوص بوحدة النيت. القيمة 203 هي مرجع HDR؛ ارفعها في غرفة مضيئة.",
     "الإضاءات",
     "إلى أي حد تتجاوز الإضاءات الأبيض. 0% يقصها كما في SDR؛ والقيم الأعلى تجعلها أسطع. لا تتغير الدرجات المتوسطة "
     "والواجهة أبداً.",
     "بداية الإضاءات",
     "حيث تبدأ الإضاءات، كنسبة مئوية من سطوع المشهد الذي تعرضه اللعبة أبيض. 100%: تنمو بسلاسة من الأبيض. أقل: ترتفع "
     "أسرع؛ أكثر: يبقى جزء أكبر أبيض كما في SDR."},
    {"HDR", "Вывод HDR",
     "Выводит изображение в HDR: блики, которые игра обрезает до белого (солнце, свечение, искры), становятся ярче, "
     "остальное выглядит как в SDR. Нужен включённый HDR Windows для этого экрана. Применится после перезапуска.",
     "Перезапустите игру, чтобы переключить вывод HDR.",
     "HDR не активен: HDR Windows выключен для этого экрана (или не поддерживается), игра показывает SDR.",
     "Пиковая яркость",
     "Максимальная яркость экрана в нитах (LG OLED: около 1000; её измеряет приложение «Калибровка HDR» Windows). Блики "
     "плавно подходят к ней.",
     "Белый лист",
     "Яркость SDR-части изображения, меню и текста в нитах. 203 — эталон HDR; в светлой комнате увеличьте.",
     "Блики",
     "Насколько блики выходят за белый. 0% обрезает их как в SDR; больше — ярче. Средние тона и интерфейс не меняются.",
     "Начало бликов",
     "Где начинаются блики, в процентах от яркости сцены, которую игра показывает белой. 100%: плавно растут из белого. "
     "Меньше: растут быстрее; больше: большая часть остаётся белой, как в SDR."},
    {"HDR", "HDR çıkışı",
     "Görüntüyü HDR olarak gönderir: oyunun beyaza kırptığı parlak noktalar (güneş, ışımalar, pırıltılar) daha parlak "
     "olur, gerisi SDR gibi kalır. Bu ekran için Windows HDR açık olmalıdır. Yeniden başlatınca uygulanır.",
     "HDR çıkışını değiştirmek için oyunu yeniden başlatın.",
     "HDR etkin değil: bu ekran için Windows HDR kapalı (veya desteklenmiyor), oyun SDR gösteriyor.",
     "En yüksek parlaklık",
     "Ekranınızın gösterebildiği en yüksek parlaklık, nit cinsinden (LG OLED: yaklaşık 1000; Windows HDR Kalibrasyonu "
     "uygulaması ölçer). Parlak noktalar ona yumuşakça yaklaşır.",
     "Kâğıt beyazı",
     "Görüntünün SDR kısmının, menülerin ve yazıların parlaklığı, nit cinsinden. 203 HDR referansıdır; aydınlık odada "
     "artırın.",
     "Parlak noktalar",
     "Parlak noktaların beyazın ne kadar ötesine geçtiği. %0 onları SDR gibi kırpar; daha yüksek daha parlak yapar. Ara "
     "tonlar ve arayüz asla değişmez.",
     "Parlaklık başlangıcı",
     "Parlak noktaların başladığı yer, oyunun beyaz gösterdiği sahne parlaklığının yüzdesi olarak. %100: beyazdan "
     "yumuşakça büyürler. Daha az: daha hızlı yükselir; daha çok: SDR gibi beyaz kalan kısım artar."},
    {"HDR", "HDR-uitvoer",
     "Stuurt het beeld in HDR: lichten die het spel tot wit afkapt (zon, gloed, glinsteringen) stralen feller, de rest "
     "blijft als in SDR. Windows-HDR moet aan staan voor dit scherm. Wordt toegepast na opnieuw starten.",
     "Start het spel opnieuw om HDR-uitvoer te wisselen.",
     "HDR is niet actief: Windows-HDR staat uit voor dit scherm (of wordt niet ondersteund), het spel toont SDR.",
     "Piekhelderheid",
     "De hoogste helderheid van je scherm in nits (LG-OLED's: ongeveer 1000; de app Windows HDR-kalibratie meet het). "
     "Lichten lopen er zacht naartoe.",
     "Papierwit",
     "Helderheid van het SDR-deel van het beeld, menu's en tekst in nits. 203 is de HDR-referentie; verhoog het in een "
     "lichte kamer.",
     "Lichten",
     "Hoe ver lichten voorbij wit gaan. 0% kapt ze af als in SDR; hoger maakt ze feller. Middentonen en de interface "
     "veranderen nooit.",
     "Begin lichten",
     "Waar de lichten beginnen, in procent van de scènehelderheid die het spel als wit toont. 100%: ze groeien zacht "
     "uit wit. Lager: ze stijgen sneller; hoger: een groter deel blijft wit, als in SDR."},
    {"HDR", "HDR 输出",
     "以 HDR 输出画面：游戏截断为白色的高光（太阳、光晕、闪光）会更亮，其余部分与 SDR 相同。需要为此显示器开启 Windows "
     "HDR。重启后生效。",
     "重新启动游戏以切换 HDR 输出。",
     "HDR 未启用：此显示器的 Windows HDR 已关闭（或不支持），游戏显示 SDR。",
     "峰值亮度",
     "显示器能显示的最高亮度（尼特）（LG OLED 约 1000；可用 Windows HDR 校准应用测量）。高光会平滑地趋近此值。",
     "纸白",
     "SDR 画面、菜单和文字的亮度（尼特）。203 是 HDR 参考值；明亮房间中可调高。",
     "高光",
     "高光超出白色的程度。0% 与 SDR 一样截断；数值越高越亮。中间调和界面始终不变。",
     "高光起点",
     "高光开始的位置，以游戏显示为白色的场景亮度的百分比表示。100%：从白色平滑增长。更低：上升更快；更高：更多部分像 SDR "
     "一样保持白色。"},
    {"HDR", "HDR 輸出",
     "以 HDR 輸出畫面：遊戲截斷為白色的高光（太陽、光暈、閃光）會更亮，其餘部分與 SDR 相同。需要為此顯示器開啟 Windows "
     "HDR。重新啟動後生效。",
     "重新啟動遊戲以切換 HDR 輸出。",
     "HDR 未啟用：此顯示器的 Windows HDR 已關閉（或不支援），遊戲顯示 SDR。",
     "峰值亮度",
     "顯示器能顯示的最高亮度（尼特）（LG OLED 約 1000；可用 Windows HDR 校正應用程式測量）。高光會平滑地趨近此值。",
     "紙白",
     "SDR 畫面、選單和文字的亮度（尼特）。203 是 HDR 參考值；明亮房間中可調高。",
     "高光",
     "高光超出白色的程度。0% 與 SDR 一樣截斷；數值越高越亮。中間調和介面始終不變。",
     "高光起點",
     "高光開始的位置，以遊戲顯示為白色的場景亮度的百分比表示。100%：從白色平滑增長。更低：上升更快；更高：更多部分像 SDR "
     "一樣保持白色。"},
};
static_assert(std::size(kHdrText) == std::size(kLabels));
// SDR look (label, description, the three choices) and the scene grading rows (label, description: Exposure,
// Contrast, Shadows, Saturation, Highlight saturation), same language order as kHdrText.
constexpr const char* kHdrGradeText[13][15] = {
    {"SDR look",
     "Imitates how an SDR display shows the game: deeper shadows and richer colour than the plain linear picture. Gamma 2.2 is typical for TVs and OLEDs, 2.4 is darker (dim rooms), sRGB turns the effect off.",
     "sRGB (off)",
     "Gamma 2.2",
     "Gamma 2.4",
     "Exposure",
     "Brightens or darkens the 3D scene (not the HUD). 100% is the game's own look.",
     "Contrast",
     "Steepens or flattens the scene's brightness curve around middle grey. 100% is the game's own look.",
     "Shadows",
     "Lifts (above 100%) or deepens (below) the darkest parts of the scene. Brighter parts do not move.",
     "Saturation",
     "Colour strength of the 3D scene, judged in a perceptual colour space so brightness holds. 0% is black and white.",
     "Highlight saturation",
     "Colour strength of the brightest parts of the scene only: below 100% they wash toward white, above they keep more colour."},
    {"Aspetto SDR",
     "Imita come uno schermo SDR mostra il gioco: ombre più profonde e colori più ricchi dell’immagine lineare. Gamma 2.2 è tipico di TV e OLED, 2.4 è più scuro (stanze buie), sRGB disattiva l’effetto.",
     "sRGB (no)",
     "Gamma 2.2",
     "Gamma 2.4",
     "Esposizione",
     "Schiarisce o scurisce la scena 3D (non l’interfaccia). 100% è l’aspetto originale.",
     "Contrasto",
     "Rende più ripida o più piatta la curva di luminosità attorno al grigio medio. 100% è l’aspetto originale.",
     "Ombre",
     "Schiarisce (sopra 100%) o scurisce (sotto) le parti più scure della scena. Le parti chiare non cambiano.",
     "Saturazione",
     "Intensità dei colori della scena 3D, misurata in uno spazio percettivo così la luminosità resta uguale. 0% è bianco e nero.",
     "Saturazione luci",
     "Intensità dei colori solo nelle parti più luminose: sotto 100% tendono al bianco, sopra mantengono più colore."},
    {"SDR-Look",
     "Ahmt nach, wie ein SDR-Bildschirm das Spiel zeigt: tiefere Schatten und kräftigere Farben als das rein lineare Bild. Gamma 2.2 ist typisch für Fernseher und OLEDs, 2.4 ist dunkler (dunkle Räume), sRGB schaltet den Effekt ab.",
     "sRGB (aus)",
     "Gamma 2.2",
     "Gamma 2.4",
     "Belichtung",
     "Hellt die 3D-Szene auf oder dunkelt sie ab (nicht die Anzeige). 100% ist der Originallook.",
     "Kontrast",
     "Macht die Helligkeitskurve um das mittlere Grau steiler oder flacher. 100% ist der Originallook.",
     "Schatten",
     "Hebt (über 100%) oder vertieft (darunter) die dunkelsten Bereiche der Szene. Hellere Bereiche bleiben gleich.",
     "Sättigung",
     "Farbstärke der 3D-Szene, in einem wahrnehmungsgerechten Farbraum, damit die Helligkeit bleibt. 0% ist Schwarzweiß.",
     "Lichtersättigung",
     "Farbstärke nur der hellsten Bereiche: unter 100% werden sie weißer, darüber behalten sie mehr Farbe."},
    {"Rendu SDR",
     "Imite l’affichage du jeu sur un écran SDR : ombres plus profondes et couleurs plus riches que l’image linéaire. Gamma 2.2 est typique des TV et OLED, 2.4 est plus sombre (pièces sombres), sRGB désactive l’effet.",
     "sRGB (non)",
     "Gamma 2.2",
     "Gamma 2.4",
     "Exposition",
     "Éclaircit ou assombrit la scène 3D (pas l’interface). 100% correspond au rendu d’origine.",
     "Contraste",
     "Rend la courbe de luminosité plus raide ou plus douce autour du gris moyen. 100% correspond au rendu d’origine.",
     "Ombres",
     "Éclaircit (au-dessus de 100%) ou assombrit (en dessous) les zones les plus sombres. Les zones claires ne bougent pas.",
     "Saturation",
     "Intensité des couleurs de la scène 3D, mesurée dans un espace perceptuel pour garder la luminosité. 0% donne du noir et blanc.",
     "Saturation des hautes lumières",
     "Intensité des couleurs des zones les plus lumineuses seulement : sous 100% elles tirent vers le blanc, au-dessus elles gardent plus de couleur."},
    {"Aspecto SDR",
     "Imita cómo muestra el juego una pantalla SDR: sombras más profundas y colores más ricos que la imagen lineal. Gamma 2.2 es lo típico en TV y OLED, 2.4 es más oscuro (habitaciones oscuras), sRGB desactiva el efecto.",
     "sRGB (no)",
     "Gamma 2.2",
     "Gamma 2.4",
     "Exposición",
     "Aclara u oscurece la escena 3D (no la interfaz). 100% es el aspecto original.",
     "Contraste",
     "Hace más pronunciada o más suave la curva de brillo alrededor del gris medio. 100% es el aspecto original.",
     "Sombras",
     "Aclara (por encima de 100%) u oscurece (por debajo) las zonas más oscuras de la escena. Las zonas claras no cambian.",
     "Saturación",
     "Intensidad del color de la escena 3D, medida en un espacio perceptual para mantener el brillo. 0% es blanco y negro.",
     "Saturación de luces",
     "Intensidad del color solo en las zonas más brillantes: por debajo de 100% tienden al blanco, por encima conservan más color."},
    {"SDR 느낌",
     "SDR 화면이 게임을 보여 주는 방식을 흉내 냅니다. 선형 이미지보다 그림자가 깊고 색이 풍부합니다. 감마 2.2는 TV와 OLED에서 일반적이고, 2.4는 더 어둡습니다(어두운 방). sRGB는 효과를 끕니다.",
     "sRGB (끔)",
     "감마 2.2",
     "감마 2.4",
     "노출",
     "3D 장면을 밝게 또는 어둡게 합니다(HUD 제외). 100%가 원래 모습입니다.",
     "대비",
     "중간 회색을 기준으로 밝기 곡선을 가파르게 또는 완만하게 합니다. 100%가 원래 모습입니다.",
     "그림자",
     "장면의 가장 어두운 부분을 밝게(100% 초과) 또는 더 어둡게(미만) 합니다. 밝은 부분은 그대로입니다.",
     "채도",
     "3D 장면의 색 강도입니다. 지각 색 공간에서 조절해 밝기는 유지됩니다. 0%는 흑백입니다.",
     "하이라이트 채도",
     "가장 밝은 부분의 색 강도만 조절합니다. 100% 미만은 흰색에 가까워지고, 초과는 색을 더 유지합니다."},
    {"SDR風の見え方",
     "SDR画面での見え方を再現します。線形の映像より影が深く、色が豊かになります。ガンマ2.2はテレビやOLEDで一般的、2.4はより暗め(暗い部屋向け)、sRGBで効果をオフにします。",
     "sRGB(オフ)",
     "ガンマ2.2",
     "ガンマ2.4",
     "露出",
     "3Dシーンを明るく、または暗くします(HUDは除く)。100%がゲーム本来の見た目です。",
     "コントラスト",
     "中間グレーを基準に明るさのカーブを急に、またはなだらかにします。100%がゲーム本来の見た目です。",
     "シャドウ",
     "シーンの最も暗い部分を持ち上げ(100%超)、または沈めます(未満)。明るい部分は変わりません。",
     "彩度",
     "3Dシーンの色の強さです。知覚的な色空間で調整するので明るさは保たれます。0%で白黒になります。",
     "ハイライトの彩度",
     "最も明るい部分だけの色の強さです。100%未満で白に近づき、超えると色が残ります。"},
    {"مظهر SDR",
     "يحاكي طريقة عرض شاشة SDR للعبة: ظلال أعمق وألوان أغنى من الصورة الخطية. جاما 2.2 شائعة في أجهزة التلفاز وشاشات OLED، و2.4 أغمق (للغرف المعتمة)، وsRGB يوقف التأثير.",
     "sRGB (إيقاف)",
     "جاما 2.2",
     "جاما 2.4",
     "التعريض",
     "يفتّح المشهد ثلاثي الأبعاد أو يعتّمه (ليس الواجهة). 100% هو مظهر اللعبة الأصلي.",
     "التباين",
     "يجعل منحنى السطوع حول الرمادي المتوسط أشد أو ألطف. 100% هو مظهر اللعبة الأصلي.",
     "الظلال",
     "يرفع (فوق 100%) أو يعمّق (تحت) أغمق أجزاء المشهد. الأجزاء الأفتح لا تتغير.",
     "التشبع",
     "قوة ألوان المشهد ثلاثي الأبعاد، مقيسة في فضاء لوني إدراكي ليبقى السطوع كما هو. 0% أبيض وأسود.",
     "تشبع الإضاءات",
     "قوة الألوان في أسطع الأجزاء فقط: تحت 100% تميل إلى الأبيض، وفوقها تحتفظ بلون أكثر."},
    {"Вид SDR",
     "Имитирует то, как игру показывает SDR-экран: более глубокие тени и насыщенные цвета, чем у линейного изображения. Гамма 2.2 типична для телевизоров и OLED, 2.4 темнее (тёмные комнаты), sRGB отключает эффект.",
     "sRGB (выкл.)",
     "Гамма 2.2",
     "Гамма 2.4",
     "Экспозиция",
     "Делает 3D-сцену светлее или темнее (не интерфейс). 100% — исходный вид игры.",
     "Контраст",
     "Делает кривую яркости вокруг среднего серого круче или положе. 100% — исходный вид игры.",
     "Тени",
     "Высветляет (выше 100%) или углубляет (ниже) самые тёмные части сцены. Светлые части не меняются.",
     "Насыщенность",
     "Сила цвета 3D-сцены в перцептивном цветовом пространстве, поэтому яркость сохраняется. 0% — чёрно-белое изображение.",
     "Насыщенность светов",
     "Сила цвета только самых ярких частей: ниже 100% они уходят в белый, выше сохраняют больше цвета."},
    {"SDR görünümü",
     "Oyunun bir SDR ekranda nasıl göründüğünü taklit eder: doğrusal görüntüden daha derin gölgeler ve daha zengin renkler. Gamma 2.2 TV ve OLED’lerde tipiktir, 2.4 daha koyudur (karanlık odalar), sRGB efekti kapatır.",
     "sRGB (kapalı)",
     "Gamma 2.2",
     "Gamma 2.4",
     "Pozlama",
     "3B sahneyi aydınlatır veya karartır (arayüz hariç). %100 oyunun kendi görünümüdür.",
     "Kontrast",
     "Orta gri etrafındaki parlaklık eğrisini dikleştirir veya yumuşatır. %100 oyunun kendi görünümüdür.",
     "Gölgeler",
     "Sahnenin en karanlık kısımlarını aydınlatır (%100 üstü) veya koyulaştırır (altı). Parlak kısımlar değişmez.",
     "Doygunluk",
     "3B sahnenin renk gücü; parlaklık korunsun diye algısal bir renk uzayında ölçülür. %0 siyah beyazdır.",
     "Parlak alan doygunluğu",
     "Yalnızca en parlak kısımların renk gücü: %100 altında beyaza kayar, üstünde daha çok renk korur."},
    {"SDR-weergave",
     "Bootst na hoe een SDR-scherm het spel toont: diepere schaduwen en rijkere kleuren dan het lineaire beeld. Gamma 2.2 is gebruikelijk voor tv’s en OLED’s, 2.4 is donkerder (donkere kamers), sRGB zet het effect uit.",
     "sRGB (uit)",
     "Gamma 2.2",
     "Gamma 2.4",
     "Belichting",
     "Maakt de 3D-scène lichter of donkerder (niet de HUD). 100% is de eigen look van het spel.",
     "Contrast",
     "Maakt de helderheidscurve rond middengrijs steiler of vlakker. 100% is de eigen look van het spel.",
     "Schaduwen",
     "Licht de donkerste delen van de scène op (boven 100%) of maakt ze dieper (eronder). Lichtere delen blijven gelijk.",
     "Verzadiging",
     "Kleurkracht van de 3D-scène, gemeten in een perceptuele kleurruimte zodat de helderheid blijft. 0% is zwart-wit.",
     "Verzadiging hooglichten",
     "Kleurkracht van alleen de helderste delen: onder 100% worden ze witter, erboven houden ze meer kleur."},
    {"SDR 观感",
     "模拟 SDR 显示器呈现游戏的方式：比线性画面阴影更深、色彩更丰富。伽马 2.2 是电视和 OLED 的常见值，2.4 更暗（适合暗室），sRGB 关闭此效果。",
     "sRGB（关）",
     "伽马 2.2",
     "伽马 2.4",
     "曝光",
     "调亮或调暗 3D 场景（不含 HUD）。100% 为游戏原始效果。",
     "对比度",
     "以中灰为中心，让亮度曲线更陡或更平缓。100% 为游戏原始效果。",
     "阴影",
     "提亮（高于 100%）或压暗（低于 100%）场景最暗的部分。较亮部分不变。",
     "饱和度",
     "3D 场景的色彩强度，在感知色彩空间中调整，亮度保持不变。0% 为黑白。",
     "高光饱和度",
     "仅调整最亮部分的色彩强度：低于 100% 趋向白色，高于 100% 保留更多色彩。"},
    {"SDR 觀感",
     "模擬 SDR 顯示器呈現遊戲的方式：比線性畫面陰影更深、色彩更豐富。伽馬 2.2 是電視和 OLED 的常見值，2.4 更暗（適合暗室），sRGB 關閉此效果。",
     "sRGB（關）",
     "伽馬 2.2",
     "伽馬 2.4",
     "曝光",
     "調亮或調暗 3D 場景（不含 HUD）。100% 為遊戲原始效果。",
     "對比度",
     "以中灰為中心，讓亮度曲線更陡或更平緩。100% 為遊戲原始效果。",
     "陰影",
     "提亮（高於 100%）或壓暗（低於 100%）場景最暗的部分。較亮部分不變。",
     "飽和度",
     "3D 場景的色彩強度，在感知色彩空間中調整，亮度保持不變。0% 為黑白。",
     "高光飽和度",
     "僅調整最亮部分的色彩強度：低於 100% 趨向白色，高於 100% 保留更多色彩。"},
};
static_assert(std::size(kHdrGradeText) == std::size(kLabels));


// HDR rows of the Video tab. The toggle applies after a restart (nvn_hdr.cpp reads it once); the other four apply live.
void hdr_rows(Settings& s, int lang) {
    const char* const* t = kHdrText[lang];
    rows::section(t[0]);
    bool on = s.hdr != 0;
    const char* toggle_desc = t[2];
    char not_active[1024];
    if (on && !nvn::hdr_output()) {  // asked for, but the swapchain is SDR (Windows HDR off, or a restart pending)
        std::snprintf(not_active, sizeof not_active, "%s\n\n%s", t[2], t[4]);
        toggle_desc = not_active;
    }
    if (rows::toggle(t[1], &on, kDefaults.hdr != 0, toggle_desc)) s.hdr = on ? 1 : 0, settings_changed(true), ui::notify(t[3]);
    if (!on) return;  // the HDR settings only show while HDR output is on, so the menu stays short without it
    rows::indent(1);
    if (!on) rows::disable_next();
    if (rows::integer(t[5], &s.hdr_peak, 400, 4000, 50, kDefaults.hdr_peak, "%d nits", t[6])) settings_changed(true);
    if (!on) rows::disable_next();
    if (rows::integer(t[7], &s.hdr_paper_white, 80, 500, 1, kDefaults.hdr_paper_white, "%d nits", t[8]))
        settings_changed(true);
    if (!on) rows::disable_next();
    {
        // The net look of an SDR display (kHdrGradeText 0..4).
        const char* const* gt = kHdrGradeText[lang];
        const char* const eotf_names[] = {gt[2], gt[3], gt[4]};
        if (rows::choice(gt[0], &s.hdr_sdr_eotf, eotf_names, 3, kDefaults.hdr_sdr_eotf, gt[1]))
            settings_changed(true);
    }
    if (!on) rows::disable_next();
    if (rows::integer(t[9], &s.hdr_highlights, 0, 300, 5, kDefaults.hdr_highlights, "%d%%", t[10])) settings_changed(true);
    if (!on || s.hdr_highlights == 0) rows::disable_next();
    if (rows::integer(t[11], &s.hdr_highlight_start, 50, 300, 5, kDefaults.hdr_highlight_start, "%d%%", t[12]))
        settings_changed(true);
    // Scene grading (kHdrGradeText 5..14). It changes the 3D picture only: the HUD, menus and title screen are not
    // graded. 100% is neutral for all of them (the game's own look).
    struct GradeRow {
        int Settings::*value;
        int lo, hi, def;
    };
    const GradeRow grade_rows[] = {
        {&Settings::hdr_exposure, 25, 300, kDefaults.hdr_exposure},
        {&Settings::hdr_contrast, 25, 200, kDefaults.hdr_contrast},
        {&Settings::hdr_shadows, 25, 175, kDefaults.hdr_shadows},
        {&Settings::hdr_saturation, 0, 300, kDefaults.hdr_saturation},
        {&Settings::hdr_highlight_saturation, 0, 300, kDefaults.hdr_highlight_saturation},
    };
    const char* const* gr = kHdrGradeText[lang];
    for (size_t k = 0; k < std::size(grade_rows); k++) {
        const GradeRow& r = grade_rows[k];
        if (rows::integer(gr[5 + 2 * k], &(s.*r.value), r.lo, r.hi, 5, r.def, "%d%%", gr[6 + 2 * k]))
            settings_changed(true);
    }
    rows::indent(-1);
}
#endif

void video(MenuContext& c, const Labels& l) {
    Settings& s = c.settings;
    const int lang = std::clamp(s.language, 0, 12);
    rows::begin("##video", c.height);
    rows::section(kCommon[lang][0]);
    constexpr const char* kAspectText[13][3] = {
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
    const char* aspect_names[] = {kAspectText[lang][2], "21:9", "32:9", "16:10"};
    int aspect = std::max(0, s.aspect_ratio);
    const char* aspect_label = kAspectText[lang][0];
    const char* aspect_desc = kAspectText[lang][1];
    if (rows::choice(aspect_label, &aspect, aspect_names, 4, 0, aspect_desc)) {
        adjust_window_size_for_aspect(aspect, s.width, s.height);
        s.aspect_ratio = aspect;
        settings_changed(true);
        ui::notify(aspect_desc);
    }
    // the window's size: four presets matching the selected aspect ratio, or the current custom size as a fifth
    const int aspect_idx = std::clamp(s.aspect_ratio, 0, 3);
    char preset_labels[4][24];
    const char* sizes[5] = {};
    char custom[32];
    int size = -1;
    for (int i = 0; i < 4; ++i) {
        const int pw = kWindowSizesByAspect[aspect_idx][i][0];
        const int ph = kWindowSizesByAspect[aspect_idx][i][1];
        std::snprintf(preset_labels[i], sizeof preset_labels[i], "%d x %d", pw, ph);
        sizes[i] = preset_labels[i];
        if (pw == s.width && ph == s.height) size = i;
    }
    int n = 4;
    if (size < 0) std::snprintf(custom, sizeof custom, "%d x %d", s.width, s.height), sizes[4] = custom, size = 4, n = 5;
    if (!c.host.can_resize || s.fullscreen) rows::disable_next();
    if (rows::choice(l.window_size, &size, sizes, n, 0, translated_desc(desc::window_size, lang)) && size < 4)
        s.width = kWindowSizesByAspect[aspect_idx][size][0],
        s.height = kWindowSizesByAspect[aspect_idx][size][1],
        settings_changed(true);
    if (rows::toggle(l.fullscreen, &s.fullscreen, kDefaults.fullscreen, translated_desc(desc::fullscreen, lang))) settings_changed(true);
    if (c.host.monitors) {
        static constexpr const char* kMonitorLabel[13] = {
            "Monitor", "Monitor", "Bildschirm", "Écran", "Monitor", "모니터", "モニター",
            "الشاشة", "Монитор", "Monitör", "Beeldscherm", "显示器", "顯示器",
        };
        static constexpr const char* kMonitorDesc[13] = {
            "Which display to show the game on.",
            "Schermo su cui mostrare il gioco.",
            "Auf welchem Bildschirm das Spiel angezeigt wird.",
            "Écran sur lequel afficher le jeu.",
            "Pantalla en la que se muestra el juego.",
            "게임을 표시할 모니터를 선택합니다.",
            "ゲームを表示するモニターを選択します。",
            "الشاشة التي تُعرض عليها اللعبة.",
            "Экран, на котором отображается игра.",
            "Oyunun gösterileceği ekran.",
            "Het beeldscherm waarop het spel wordt weergegeven.",
            "选择显示游戏的显示器。",
            "選擇顯示遊戲的顯示器。",
        };
        std::vector<std::string> mons = c.host.monitors();
        if (!mons.empty()) {
            std::vector<const char*> mon_ptrs;
            mon_ptrs.reserve(mons.size());
            for (const auto& m : mons) mon_ptrs.push_back(m.c_str());
            int mon = std::clamp(s.monitor, 0, int(mons.size()) - 1);
            if (rows::choice(kMonitorLabel[lang], &mon, mon_ptrs.data(), int(mon_ptrs.size()), 0, kMonitorDesc[lang])) {
                s.monitor = mon;
                settings_changed(true);
            }
        }
    }
    if (!c.host.can_always_on_top) rows::disable_next();
    if (rows::toggle(l.topmost, &s.always_on_top, kDefaults.always_on_top, translated_desc(desc::topmost, lang))) settings_changed(true);
    rows::section(kCommon[lang][1]);
    static constexpr const char* kNativeLabel[13] = {
        "Native", "Nativa", "Nativ", "Native", "Nativa", "네이티브", "ネイティブ",
        "أصلية", "Экранное", "Doğal", "Schermresolutie", "屏幕原生", "螢幕原生",
    };
    uint32_t dw = 0, dh = 0;
    nx::native_display_size(dw, dh);
    if (!dw || !dh) dw = s.width > 0 ? uint32_t(s.width) : 1920, dh = s.height > 0 ? uint32_t(s.height) : 1080;
    char native_opt[64];
    std::snprintf(native_opt, sizeof native_opt, "%s (%ux%u)", kNativeLabel[lang], dw, dh);
    const char* internal_opts[10];
    for (int i = 0; i < 9; ++i) internal_opts[i] = kInternal[lang][i];
    internal_opts[9] = native_opt;
    int internal_ui = 0;
    for (int i = 0; i < 10; ++i)
        if (kInternalOrder[i] == s.internal_resolution) internal_ui = i;
    if (rows::choice(l.internal_resolution, &internal_ui, internal_opts, 10, 0,
                     translated_desc(desc::internal, lang)))
        s.internal_resolution = kInternalOrder[internal_ui], settings_changed(true), ui::notify(l.restart_note);
    const char* upscalers[] = {"Bilinear", "FSR 1"};
    if (rows::choice(kCommon[lang][3], &s.upscaler, upscalers, 2, kDefaults.upscaler, translated_desc(desc::upscaler, lang))) settings_changed(true);
    rows::indent(1);
    if (s.upscaler != 1) rows::disable_next();
    int sharp = 200 - s.fsr_sharpness;  // shown as sharpness: 0 soft .. 200 sharp
    if (rows::integer(kCommon[lang][4], &sharp, 0, 200, 5, 200 - kDefaults.fsr_sharpness, "%d%%", translated_desc(desc::sharpness, lang)))
        s.fsr_sharpness = 200 - sharp, settings_changed(true);
    rows::indent(-1);
    const char* aa[] = {"Off", "FXAA", "SMAA"};
    if (rows::choice(kCommon[lang][5], &s.antialiasing, aa, 3, kDefaults.antialiasing, translated_desc(desc::antialiasing, lang)))
        settings_changed(true);
    const char* af_names[] = {"Game default", "2x", "4x", "8x", "16x"};
    const int af_levels[] = {0, 2, 4, 8, 16};
    int af = 0;
    for (int i = 0; i < 5; ++i)
        if (af_levels[i] == s.anisotropy) af = i;
    if (rows::choice(kCommon[lang][6], &af, af_names, 5, 0, translated_desc(desc::anisotropy, lang)))
        s.anisotropy = af_levels[af], settings_changed(true), ui::notify(l.restart_note);
    {
        constexpr const char* kShadowText[13][2] = {
            {"Shadow resolution", "Multiplies shadow map resolution (2x/4x smooths jagged shadow edges). Applies after restarting the game."},
            {"Risoluzione ombre", "Moltiplica la risoluzione delle shadow map (2x/4x elimina le seghettature delle ombre). Si applica al riavvio."},
            {"Schattenauflösung", "Multipliziert die Shadow-Map-Auflösung (2x/4x glättet ausgefranste Schattenkanten). Wird nach Neustart aktiv."},
            {"Résolution des ombres", "Multiplie la résolution des ombres (2x/4x lisse les bords crénelés). S'applique après redémarrage."},
            {"Resolución de sombras", "Multiplica la resolución de sombras (2x/4x suaviza los bordes dentados). Se aplica al reiniciar."},
            {"그림자 해상도", "그림자 맵 해상도를 배율로 높입니다 (2x/4x는 계단 현상을 줄입니다). 재시작 후 적용됩니다."},
            {"シャドウ解像度", "シャドウマップ解像度を倍率で上げます（2x/4xでジャギーを軽減）。再起動後に適用されます。"},
            {"دقة الظلال", "يضاعف دقة خريطة الظلال (2x/4x ينعّم الحواف المتعرجة). يُطبّق بعد إعادة التشغيل."},
            {"Разрешение теней", "Умножает разрешение карт теней (2x/4x сглаживает лесенки). Применяется после перезапуска."},
            {"Gölge çözünürlüğü", "Gölge haritası çözünürlüğünü çarpar (2x/4x tırtıklı kenarları yumuşatır). Yeniden başlatınca uygulanır."},
            {"Schaduwresolutie", "Vermenigvuldigt de schaduwresolutie (2x/4x maakt gekartelde randen gladder). Geldt na herstart."},
            {"阴影分辨率", "倍增阴影贴图分辨率（2x/4x 可平滑锯齿边缘）。重启游戏后生效。"},
            {"陰影解析度", "倍增陰影貼圖解析度（2x/4x 可平滑鋸齒邊緣）。重啟遊戲後生效。"},
        };
        const char* shadow_label = kShadowText[lang][0];
        const char* shadow_desc = kShadowText[lang][1];
        const char* shadow_names[] = {"1x", "2x", "4x"};
        const int shadow_levels[] = {1, 2, 4};
        int sh = 0;
        for (int i = 0; i < 3; ++i)
            if (shadow_levels[i] == s.shadow_scale) sh = i;
        if (rows::choice(shadow_label, &sh, shadow_names, 3, 0, shadow_desc))
            s.shadow_scale = shadow_levels[sh], settings_changed(true), ui::notify(l.restart_note);
    }
#if !defined(__ANDROID__) && !defined(__APPLE__)
    hdr_rows(s, lang);
#endif
    rows::section(kCommon[lang][2]);
    const char* vsync_names[] = {"On", "Fast", "Adaptive", "Off"};
    if (rows::choice(kCommon[lang][7], &s.vsync, vsync_names, 4, kDefaults.vsync, translated_desc(desc::vsync, lang))) settings_changed(true);
    const char* caps[] = {"30 fps", "60 fps", "90 fps", "120 fps", "144 fps", "240 fps"};
    constexpr int cap_values[] = {30, 60, 90, 120, 144, 240};
    int cap = 1;
    for (int i = 0; i < 6; ++i)
        if (cap_values[i] == s.frame_cap) cap = i;
    if (rows::choice(l.frame_cap_label, &cap, caps, 6, 1, translated_desc(desc::frame_cap, lang)))
        s.frame_cap = cap_values[cap], settings_changed(true);
    rows::section(kExtraUiText[lang].reset_sec);
    {
        char rlabel[64];
        std::snprintf(rlabel, sizeof rlabel, kExtraUiText[lang].reset_tab_fmt, l.video);
        if (rows::action(rlabel, "", kExtraUiText[lang].reset_tab_desc)) {
            s.width = kDefaults.width, s.height = kDefaults.height, s.aspect_ratio = kDefaults.aspect_ratio;
            s.internal_resolution = kDefaults.internal_resolution, s.fullscreen = kDefaults.fullscreen;
            s.always_on_top = kDefaults.always_on_top, s.upscaler = kDefaults.upscaler;
            s.fsr_sharpness = kDefaults.fsr_sharpness, s.antialiasing = kDefaults.antialiasing;
            s.anisotropy = kDefaults.anisotropy, s.shadow_scale = kDefaults.shadow_scale, s.vsync = kDefaults.vsync, s.frame_cap = kDefaults.frame_cap;
            s.hdr = kDefaults.hdr, s.hdr_peak = kDefaults.hdr_peak, s.hdr_paper_white = kDefaults.hdr_paper_white;
            s.hdr_highlights = kDefaults.hdr_highlights, s.hdr_highlight_start = kDefaults.hdr_highlight_start;
            s.hdr_sdr_eotf = kDefaults.hdr_sdr_eotf;
            s.hdr_exposure = kDefaults.hdr_exposure, s.hdr_contrast = kDefaults.hdr_contrast;
            s.hdr_shadows = kDefaults.hdr_shadows, s.hdr_saturation = kDefaults.hdr_saturation;
            s.hdr_highlight_saturation = kDefaults.hdr_highlight_saturation;
            settings_changed(true);
            ui::notify(kExtraUiText[lang].reset_tab_note);
        }
    }
    rows::page_note(l.restart_note);
    rows::end();
}

void audio(MenuContext& c, const Labels& l) {
    Settings& s = c.settings;
    const int lang = std::clamp(s.language, 0, 12);
    rows::begin("##audio", c.height);
    rows::section(l.audio);
    if (rows::integer(l.volume, &s.volume, 0, 200, 5, kDefaults.volume, "%d%%", translated_desc(desc::volume, lang))) settings_changed(true);
    if (rows::toggle(l.mute, &s.muted, kDefaults.muted, translated_desc(desc::mute, lang))) settings_changed(true);
    {
        constexpr const char* kBgMuteText[13][2] = {
            {"Mute in background", "Automatically mutes game audio when the window loses focus."},
            {"Silenzia in background", "Silenzia automaticamente l'audio quando la finestra del gioco perde il focus."},
            {"Im Hintergrund stummschalten", "Schaltet das Spielaudio automatisch stumm, wenn das Fenster den Fokus verliert."},
            {"Muet en arrière-plan", "Coupe automatiquement le son du jeu lorsque la fenêtre perd le focus."},
            {"Silenciar en segundo plano", "Silencia automáticamente el audio del juego cuando la ventana pierde el foco."},
            {"백그라운드 음소거", "창이 포커스를 잃으면 게임 오디오를 자동으로 음소거합니다."},
            {"バックグラウンド時にミュート", "ウィンドウが非アクティブになると自動でミュートします。"},
            {"كتم الصوت في الخلفية", "يكتم صوت اللعبة تلقائياً عندما تفقد النافذة التركيز."},
            {"Без звука в фоне", "Автоматически отключает звук, когда окно теряет фокус."},
            {"Arka planda sessize al", "Pencere odağı kaybettiğinde oyun sesini otomatik olarak kapatır."},
            {"Dempen op achtergrond", "Dempt het spelgeluid automatisch wanneer het venster de focus verliest."},
            {"后台静音", "窗口失去焦点时自动静音游戏音频。"},
            {"背景靜音", "視窗失去焦點時自動靜音遊戲音訊。"},
        };
        const char* bg_mute_label = kBgMuteText[lang][0];
        const char* bg_mute_desc = kBgMuteText[lang][1];
        if (rows::toggle(bg_mute_label, &s.mute_in_background, kDefaults.mute_in_background, bg_mute_desc))
            settings_changed(true);
    }
    constexpr const char* kAudioOutText[13][8] = {
        {"Audio output", "Stereo, Mono (same mix on both speakers), or 5.1 PCM Surround (5.1 requires a restart).",
         "Restart the game to apply 5.1 output.", "Stereo", "Mono", "5.1 Surround (PCM)",
         "Stereo separation", "Controls how wide sound effects are panned between left and right speakers."},
        {"Uscita audio", "Stereo, Mono (stesso audio su entrambe le casse) o 5.1 Surround PCM (richiede riavvio).",
         "Riavvia il gioco per applicare il 5.1.", "Stereo", "Mono", "5.1 Surround (PCM)",
         "Separazione stereo", "Riduce quanto gli effetti sonori vengono spostati solo su una cassa."},
        {"Audioausgabe", "Stereo, Mono (gleicher Ton auf beiden Lautsprechern) oder 5.1 PCM Surround (erfordert Neustart).",
         "Starte das Spiel neu, um 5.1 anzuwenden.", "Stereo", "Mono", "5.1 Surround (PCM)",
         "Stereotrennung", "Regelt, wie stark Soundeffekte zwischen links und rechts verteilt werden."},
        {"Sortie audio", "Stéréo, Mono (même son des deux côtés) ou Surround 5.1 PCM (redémarrage requis pour le 5.1).",
         "Redémarrez le jeu pour appliquer le 5.1.", "Stéréo", "Mono", "Surround 5.1 (PCM)",
         "Séparation stéréo", "Règle l'amplitude de la spatialisation gauche-droite des effets sonores."},
        {"Salida de audio", "Estéreo, Mono (misma mezcla en ambos altavoces) o 5.1 PCM Surround (requiere reiniciar).",
         "Reinicia el juego para aplicar la salida 5.1.", "Estéreo", "Mono", "5.1 Surround (PCM)",
         "Separación estéreo", "Controla qué tanto se distribuyen los efectos entre izquierda y derecha."},
        {"오디오 출력", "스테레오, 모노(양쪽 스피커 동일 출력), 또는 5.1 PCM 서라운드(5.1은 재시작 필요).",
         "5.1 출력을 적용하려면 게임을 다시 시작하세요.", "스테레오", "모노", "5.1 서라운드 (PCM)",
         "스테레오 분리도", "효과음이 좌우 스피커로 퍼지는 폭을 조절합니다."},
        {"オーディオ出力", "ステレオ、モノラル（左右同じ音）、または 5.1ch PCM サラウンド（5.1ch は再起動が必要）。",
         "5.1ch 出力を適用するにはゲームを再起動してください。", "ステレオ", "モノラル", "5.1ch サラウンド (PCM)",
         "ステレオ分離", "効果音の左右の広がり具合を調整します。"},
        {"إخراج الصوت", "ستيريو، أحادي (نفس الصوت في السماعتين)، أو محيطي 5.1 PCM (يتطلب إعادة التشغيل).",
         "أعد تشغيل اللعبة لتطبيق إخراج 5.1.", "ستيريو", "أحادي", "محيطي 5.1 (PCM)",
         "فصل الستيريو", "يتحكم في مدى توزيع المؤثرات الصوتية بين السماعة اليسرى واليمنى."},
        {"Вывод звука", "Стерео, Моно (одинаковый звук в обоих каналах) или 5.1 PCM Surround (нужен перезапуск).",
         "Перезапустите игру для применения 5.1.", "Стерео", "Моно", "5.1 Surround (PCM)",
         "Разделение стерео", "Регулирует ширину панорамирования эффектов между левым и правым каналами."},
        {"Ses çıkışı", "Stereo, Mono (iki hoparlörde aynı ses) veya 5.1 PCM Surround (yeniden başlatma gerekir).",
         "5.1 çıkışı uygulamak için oyunu yeniden başlatın.", "Stereo", "Mono", "5.1 Surround (PCM)",
         "Stereo ayrımı", "Ses efektlerinin sol ve sağ hoparlörler arasındaki yayılımını ayarlar."},
        {"Audio-uitvoer", "Stereo, Mono (gelijk geluid op beide speakers) of 5.1 PCM Surround (herstart vereist voor 5.1).",
         "Herstart het spel om 5.1-uitvoer toe te passen.", "Stereo", "Mono", "5.1 Surround (PCM)",
         "Stereoscheiding", "Bepaalt hoe breed geluidseffecten tussen links en rechts worden verdeeld."},
        {"音频输出", "立体声、单声道（左右声道相同）或 5.1 PCM 环绕声（5.1 需要重启）。",
         "重启游戏以应用 5.1 输出。", "立体声", "单声道", "5.1 环绕声 (PCM)",
         "立体声分离度", "控制音效在左右声道之间的声像宽度。"},
        {"音訊輸出", "立體聲、單聲道（左右聲道相同）或 5.1 PCM 環繞聲（5.1 需要重新啟動）。",
         "重新啟動遊戲以套用 5.1 輸出。", "立體聲", "單聲道", "5.1 環繞聲 (PCM)",
         "立體聲分離度", "控制音效在左右聲道之間的聲像寬度。"},
    };
    const char* audio_modes[] = {kAudioOutText[lang][3], kAudioOutText[lang][4], kAudioOutText[lang][5]};
    const char* mode_label = kAudioOutText[lang][0];
    const char* mode_desc = kAudioOutText[lang][1];
    const int prev_mode = s.audio_mode;
    if (rows::choice(mode_label, &s.audio_mode, audio_modes, 3, kDefaults.audio_mode, mode_desc)) {
        settings_changed(true);
        if ((prev_mode == 2) != (s.audio_mode == 2))
            ui::notify(kAudioOutText[lang][2]);
    }
    rows::indent(1);
    if (s.audio_mode != 0) rows::disable_next();
    const char* sep_label = kAudioOutText[lang][6];
    const char* sep_desc = kAudioOutText[lang][7];
    if (rows::integer(sep_label, &s.stereo_separation, 0, 100, 5, kDefaults.stereo_separation, "%d%%", sep_desc))
        settings_changed(true);
    rows::indent(-1);
    rows::section(l.game_language);
    const int game_lang_count = overlay::game_version() == "1.2.1" ? int(std::size(kGameLanguageNames)) : 16;
    if (rows::choice(l.game_language, &s.game_language, kGameLanguageNames, game_lang_count,
                     kDefaults.game_language, translated_desc(desc::game_language, lang)))
        settings_changed(true), ui::notify(l.game_language_restart);
    rows::section(kExtraUiText[lang].reset_sec);
    {
        char rlabel[64];
        std::snprintf(rlabel, sizeof rlabel, kExtraUiText[lang].reset_tab_fmt, l.audio);
        if (rows::action(rlabel, "", kExtraUiText[lang].reset_tab_desc)) {
            s.volume = kDefaults.volume, s.muted = kDefaults.muted, s.mute_in_background = kDefaults.mute_in_background;
            s.audio_mode = kDefaults.audio_mode, s.stereo_separation = kDefaults.stereo_separation;
            s.game_language = kDefaults.game_language;
            settings_changed(true);
            ui::notify(kExtraUiText[lang].reset_tab_note);
        }
    }
    rows::page_note(l.game_language_restart);
    rows::end();
}

// Multiplayer input labels and help, in the same language order as kLabels.
struct InputText {
    const char *separate, *separate_help, *automatic, *none, *keyboard, *offline,
               *player, *player_help, *configure, *configure_help, *press_key,
               *icons, *icons_help, *icons_restart;
};
constexpr InputText kInputText[] = {
    {"Keyboard as separate player", "Keyboard is player 1; controllers start at player 2.", "Automatic", "None", "Keyboard", "offline", "Player", "Choose this player's input device.", "Configure controller", "Choose which controller to configure.", "Press a key...", "Button icons", "In-game button icon style synced with Pad 1's bindings. Requires restart.", "Restart the game to apply button icons."},
    {"Tastiera giocatore separato", "Tastiera: giocatore 1; controller: dal giocatore 2.", "Automatico", "Nessuno", "Tastiera", "disattivato", "Giocatore", "Seleziona il dispositivo assegnato al giocatore.", "Configura controller", "Scegli quale controller modificare dal menu a tendina.", "Premi un tasto...", "Icone tasti HUD", "Stile delle icone dei tasti in gioco sincronizzato coi comandi del Pad 1. Richiede riavvio.", "Riavvia il gioco per applicare le icone dei tasti."},
    {"Tastatur als eigener Spieler", "Tastatur: Spieler 1; Controller: ab Spieler 2.", "Automatisch", "Keines", "Tastatur", "nicht verbunden", "Spieler", "Eingabegerät für diesen Spieler wählen.", "Controller konfigurieren", "Controller zum Bearbeiten wählen.", "Taste drücken...", "Tastensymbole", "Symbolstil der Tasten im Spiel, abgestimmt auf die Belegung von Pad 1. Neustart erforderlich.", "Spiel neu starten, um Tastensymbole anzuwenden."},
    {"Clavier comme joueur séparé", "Clavier : joueur 1 ; manettes : à partir du joueur 2.", "Automatique", "Aucun", "Clavier", "déconnecté", "Joueur", "Choisissez le périphérique de ce joueur.", "Configurer la manette", "Choisissez la manette à modifier.", "Appuyez sur une touche...", "Icônes des touches", "Style des icônes en jeu synchronisé avec les touches du Pad 1. Redémarrage requis.", "Redémarrez le jeu pour appliquer les icônes."},
    {"Teclado como jugador independiente", "Teclado: jugador 1; mandos: desde el jugador 2.", "Automático", "Ninguno", "Teclado", "desconectado", "Jugador", "Elige el dispositivo de este jugador.", "Configurar mando", "Elige el mando que quieres modificar.", "Pulsa una tecla...", "Iconos de botones", "Estilo de los iconos en el juego sincronizado con los controles del Pad 1. Requiere reiniciar.", "Reinicia el juego para aplicar los iconos."},
    {"키보드를 별도 플레이어로", "키보드는 플레이어 1, 컨트롤러는 플레이어 2부터 사용합니다.", "자동", "없음", "키보드", "연결 안 됨", "플레이어", "이 플레이어의 입력 장치를 선택하세요.", "컨트롤러 설정", "설정할 컨트롤러를 선택하세요.", "키를 누르세요...", "버튼 표시", "게임 내 버튼 표시를 Pad 1 설정으로 적용합니다. 재시작 필요.", "버튼 표시를 적용하려면 게임을 다시 시작하세요."},
    {"キーボードを別のプレイヤーに", "キーボードはプレイヤー1、コントローラーは2から使用します。", "自動", "なし", "キーボード", "未接続", "プレイヤー", "このプレイヤーの入力機器を選択します。", "コントローラーを設定", "設定するコントローラーを選択します。", "キーを押してください...", "ボタンアイコン", "ゲーム内のボタン表示を Pad 1 の設定に合わせます。再起動が必要です。", "ボタンアイコンの適用には再起動が必要です。"},
    {"لوحة المفاتيح كلاعب منفصل", "لوحة المفاتيح للاعب الأول، ووحدات التحكم بدءاً من اللاعب الثاني.", "تلقائي", "لا شيء", "لوحة المفاتيح", "غير متصل", "اللاعب", "اختر جهاز الإدخال لهذا اللاعب.", "إعداد وحدة التحكم", "اختر وحدة التحكم التي تريد إعدادها.", "اضغط مفتاحاً...", "أيقونات الأزرار", "نمط أيقونات الأزرار في اللعبة متزامن مع إعدادات Pad 1. يتطلب إعادة التشغيل.", "أعد تشغيل اللعبة لتطبيق أيقونات الأزرار."},
    {"Клавиатура как отдельный игрок", "Клавиатура — игрок 1, контроллеры — начиная с игрока 2.", "Автоматически", "Нет", "Клавиатура", "не подключён", "Игрок", "Выберите устройство ввода для этого игрока.", "Настроить контроллер", "Выберите контроллер для настройки.", "Нажмите клавишу...", "Иконки кнопок", "Стиль значков кнопок в игре с учётом раскладки Pad 1. Нужен перезапуск.", "Перезапустите игру для применения иконок."},
    {"Klavye ayrı oyuncu olsun", "Klavye oyuncu 1, kumandalar oyuncu 2'den başlar.", "Otomatik", "Yok", "Klavye", "bağlı değil", "Oyuncu", "Bu oyuncunun giriş cihazını seçin.", "Kumandayı yapılandır", "Yapılandırılacak kumandayı seçin.", "Bir tuşa basın...", "Düğme simgeleri", "Oyun içi düğme simgeleri Pad 1 atamalarına göre gösterilir. Yeniden başlatma gerekir.", "Düğme simgelerini uygulamak için oyunu yeniden başlatın."},
    {"Toetsenbord als aparte speler", "Toetsenbord: speler 1; controllers vanaf speler 2.", "Automatisch", "Geen", "Toetsenbord", "niet verbonden", "Speler", "Kies het invoerapparaat voor deze speler.", "Controller instellen", "Kies welke controller je wilt instellen.", "Druk op een toets...", "Knopiconen", "Stijl van knopiconen in het spel, afgestemd op Pad 1. Herstart vereist.", "Herstart het spel om knopiconen toe te passen."},
    {"键盘作为独立玩家", "键盘为玩家1，控制器从玩家2开始。", "自动", "无", "键盘", "未连接", "玩家", "选择此玩家使用的输入设备。", "配置控制器", "选择要配置的控制器。", "请按一个键...", "按键图示", "与控制器 1 配置同步的游戏内按键图示。需要重启。", "重启游戏以应用按键图示。"},
    {"鍵盤作為獨立玩家", "鍵盤為玩家1，控制器從玩家2開始。", "自動", "無", "鍵盤", "未連線", "玩家", "選擇此玩家使用的輸入裝置。", "設定控制器", "選擇要設定的控制器。", "請按一個鍵...", "按鍵圖示", "與控制器 1 設定同步的遊戲內按鍵圖示。需要重新啟動。", "重新啟動遊戲以套用按鍵圖示。"},
};
static_assert(std::size(kInputText) == std::size(kLabels));
// Translated directional pad labels and PlayStation face-button names.
constexpr const char* kInputDirections[13][8] = {
    {"D-pad Left", "D-pad Up", "D-pad Right", "D-pad Down", "Cross", "Circle", "Square", "Triangle"},
    {"Croce sinistra", "Croce su", "Croce destra", "Croce giù", "Croce", "Cerchio", "Quadrato", "Triangolo"},
    {"Steuerkreuz links", "Steuerkreuz hoch", "Steuerkreuz rechts", "Steuerkreuz runter", "Kreuz", "Kreis", "Quadrat", "Dreieck"},
    {"Croix gauche", "Croix haut", "Croix droite", "Croix bas", "Croix", "Cercle", "Carré", "Triangle"},
    {"Cruceta izquierda", "Cruceta arriba", "Cruceta derecha", "Cruceta abajo", "Cruz", "Círculo", "Cuadrado", "Triángulo"},
    {"방향키 왼쪽", "방향키 위", "방향키 오른쪽", "방향키 아래", "십자", "원", "네모", "세모"},
    {"十字キー左", "十字キー上", "十字キー右", "十字キー下", "クロス", "サークル", "スクエア", "トライアングル"},
    {"الاتجاه يسار", "الاتجاه أعلى", "الاتجاه يمين", "الاتجاه أسفل", "صليب", "دائرة", "مربع", "مثلث"},
    {"Крестовина влево", "Крестовина вверх", "Крестовина вправо", "Крестовина вниз", "Крест", "Круг", "Квадрат", "Треугольник"},
    {"Yön tuşu sol", "Yön tuşu yukarı", "Yön tuşu sağ", "Yön tuşu aşağı", "Çarpı", "Daire", "Kare", "Üçgen"},
    {"D-pad links", "D-pad omhoog", "D-pad rechts", "D-pad omlaag", "Kruis", "Cirkel", "Vierkant", "Driehoek"},
    {"方向键左", "方向键上", "方向键右", "方向键下", "叉", "圆", "方块", "三角"},
    {"方向鍵左", "方向鍵上", "方向鍵右", "方向鍵下", "叉", "圓", "方塊", "三角"},
};


void controls(MenuContext& c, const Labels& l) {
    Settings& s = c.settings;
    const int lang = std::clamp(s.language, 0, 12);
    const InputText& t = kInputText[lang];
    rows::begin("##controls", c.height);
    rows::section(kHeaders[lang][1]);
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(_WIN32)
    if (rows::toggle(t.separate, &s.keyboard_separate, false, t.separate_help)) settings_changed(true);
    const auto device_names = plat::controller_names();
    std::array<std::string, 4> device_labels;
    const char* options[7] = {t.automatic, t.none, t.keyboard, nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < 4; ++i) {
        device_labels[i] = "Pad " + std::to_string(i + 1) + (device_names[i].empty() ? std::string(" (") + t.offline + ")" : ": " + device_names[i]);
        options[i + 3] = device_labels[i].c_str();
    }
    for (int player = 0; player < 4; ++player) {
        char label[48]; std::snprintf(label, sizeof label, "%s %d", t.player, player + 1);
        int old = s.player_devices[player];
        if (rows::choice(label, &s.player_devices[player], options, 7, 0,
                         t.player_help)) {
            const int selected = s.player_devices[player];
            if (selected >= 2) for (int other = 0; other < 4; ++other)
                if (other != player && s.player_devices[other] == selected) s.player_devices[other] = old >= 2 ? old : 1;
            settings_changed(true);
        }
    }
    static int edit_slot = 0;
    std::array<std::string, 4> slot_strings;
    const char* slots[4] = {};
    for (int i = 0; i < 4; ++i) {
        slot_strings[i] = "Pad " + std::to_string(i + 1) +
                          (device_names[i].empty() ? std::string(" (") + t.offline + ")" : ": " + device_names[i]);
        slots[i] = slot_strings[i].c_str();
    }
    rows::dropdown(t.configure, &edit_slot, slots, 4, t.configure_help);
    rows::section(slots[edit_slot]);
#else
    const int edit_slot = 0;
#endif
    const char* presets[] = {"Xbox / DualSense", "Nintendo"};
    int slot_preset = controller_preset_for_slot(s, size_t(edit_slot));
    if (rows::choice(l.control_preset, &slot_preset, presets, 2, kDefaults.controller_preset, translated_desc(desc::preset, lang))) {
        if (edit_slot == 0) s.controller_preset = s.pad_presets[0] = slot_preset;
        else s.pad_presets[size_t(edit_slot)] = slot_preset;
        settings_changed(true);
    }
    const char* icon_styles[] = {t.automatic, "Nintendo", "Xbox", "PlayStation"};
    if (rows::choice(t.icons, &s.button_icons, icon_styles, 4, kDefaults.button_icons, t.icons_help))
        settings_changed(true), ui::notify(t.icons_restart);
    if (rows::integer(kCommon[lang][8], &s.rumble, 0, 100, 10, kDefaults.rumble, "%d%%", translated_desc(desc::rumble, lang))) settings_changed(true);
    {
        constexpr const char* kDeadzoneText[13][4] = {
            {"Left stick deadzone", "Ignores small movements near the center of the left analog stick to prevent drift.",
             "Right stick deadzone", "Ignores small movements near the center of the right analog stick to prevent drift."},
            {"Zona morta stick L", "Ignora i piccoli movimenti al centro dello stick sinistro per evitare il drift.",
             "Zona morta stick R", "Ignora i piccoli movimenti al centro dello stick destro per evitare il drift."},
            {"Totzone linker Stick", "Ignoriert kleine Bewegungen in der Mitte des linken Sticks gegen Drift.",
             "Totzone rechter Stick", "Ignoriert kleine Bewegungen in der Mitte des rechten Sticks gegen Drift."},
            {"Zone morte stick G", "Ignore les petits mouvements au centre du stick gauche pour éviter le drift.",
             "Zone morte stick D", "Ignore les petits mouvements au centre du stick droit pour éviter le drift."},
            {"Zona muerta stick I", "Ignora pequeños movimientos en el centro del stick izquierdo para evitar drift.",
             "Zona muerta stick D", "Ignora pequeños movimientos en el centro del stick derecho para evitar drift."},
            {"왼쪽 스틱 데드존", "쏠림을 방지하기 위해 왼쪽 아날로그 스틱 중앙의 미세한 움직임을 무시합니다.",
             "오른쪽 스틱 데드존", "쏠림을 방지하기 위해 오른쪽 아날로그 스틱 중앙의 미세한 움직임을 무시합니다."},
            {"左スティックデッドゾーン", "ドリフトを防ぐため左スティック中央付近の微小な入力を無視します。",
             "右スティックデッドゾーン", "ドリフトを防ぐため右スティック中央付近の微小な入力を無視します。"},
            {"المنطقة الميتة للعصا اليسرى", "يتجاهل الحركات الطفيفة في منتصف العصا اليسرى لمنع الانحراف.",
             "المنطقة الميتة للعصا اليمنى", "يتجاهل الحركات الطفيفة في منتصف العصا اليمنى لمنع الانحراف."},
            {"Мёртвая зона левого стика", "Игнорирует мелкие отклонения в центре левого стика для защиты от дрифта.",
             "Мёртвая зона правого стика", "Игнорирует мелкие отклонения в центре правого стика для защиты от дрифта."},
            {"Sol çubuk ölü bölgesi", "Sapmayı önlemek için sol analog çubuğun merkezindeki küçük hareketleri yok sayar.",
             "Sağ çubuk ölü bölgesi", "Sapmayı önlemek için sağ analog çubuğun merkezindeki küçük hareketleri yok sayar."},
            {"Dode zone linkerstick", "Negeert kleine bewegingen rond het midden van de linkerstick tegen drift.",
             "Dode zone rechterstick", "Negeert kleine bewegingen rond het midden van de rechterstick tegen drift."},
            {"左摇杆死区", "忽略左摇杆中心附近的微小移动以防止漂移。",
             "右摇杆死区", "忽略右摇杆中心附近的微小移动以防止漂移。"},
            {"左搖桿死區", "忽略左搖桿中心附近的微小移動以防止飄移。",
             "右搖桿死區", "忽略右搖桿中心附近的微小移動以防止飄移。"},
        };
        const char* dz_l_label = kDeadzoneText[lang][0];
        const char* dz_l_desc = kDeadzoneText[lang][1];
        if (rows::integer(dz_l_label, &s.pad_left_deadzones[size_t(edit_slot)], 0, 34, 1,
                          kDefaults.pad_left_deadzones[size_t(edit_slot)], "%d%%", dz_l_desc))
            settings_changed(true);
        const char* dz_r_label = kDeadzoneText[lang][2];
        const char* dz_r_desc = kDeadzoneText[lang][3];
        if (rows::integer(dz_r_label, &s.pad_right_deadzones[size_t(edit_slot)], 0, 34, 1,
                          kDefaults.pad_right_deadzones[size_t(edit_slot)], "%d%%", dz_r_desc))
            settings_changed(true);
    }
    std::array<std::string, 4> face_labels;
    const char* physical[kControlCount];
    for (size_t i = 0; i < kControlCount; ++i) physical[i] = kPhysical[i];
    constexpr const char* face_letters[] = {"A", "B", "X", "Y"};
    for (size_t i = 0; i < 4; ++i) {
        face_labels[i] = std::string(face_letters[i]) + " / " + kInputDirections[lang][i + 4];
        physical[i] = face_labels[i].c_str();
        physical[12 + i] = kInputDirections[lang][i];
    }
    for (size_t a = 0; a < kControlCount; ++a) {
        int phys = controller_binding_for_slot(s, size_t(edit_slot), a);
        const int def = default_controller_binding(a, slot_preset);
        char id[128];
        std::snprintf(id, sizeof id, "%s###pad%zu", a >= 12 ? kInputDirections[lang][a - 12] : kActions[a], a);
        if (rows::choice(id, &phys, physical, int(kControlCount), def, translated_desc(desc::pad, lang))) {
            if (edit_slot == 0) {
                s.controller_bindings[a] = phys;
                s.controller_custom_mask |= 1u << a;
                if (phys == def) s.controller_custom_mask &= ~(1u << a);
            } else {
                s.pad_bindings[size_t(edit_slot)][a] = phys;
                s.pad_custom_mask[size_t(edit_slot)] |= 1u << a;
                if (phys == def) s.pad_custom_mask[size_t(edit_slot)] &= ~(1u << a);
            }
            settings_changed(true);
        }
    }
    rows::section(kHeaders[lang][0]);
    for (size_t a = 0; a < kControlCount; ++a) {
        char id[128];
        std::snprintf(id, sizeof id, "%s###key%zu", a >= 12 ? kInputDirections[lang][a - 12] : kActions[a], a);
        const bool waiting = c.capture_action == int(a);
        const char* name = waiting ? t.press_key : c.host.key_name ? c.host.key_name(s.keyboard_bindings[a]) : "?";
        if (rows::action(id, name, translated_desc(desc::key, lang), true, waiting)) c.capture_action = int(a);  // overlay.cpp takes the next key
    }
    {
        constexpr const char* kHotkeyText[13][6] = {
            {"Hotkeys", "In-game menu", "Screenshot", "Fullscreen", "Fast-forward",
             "Press to rebind this shortcut (Esc cancels)."},
            {"Tasti rapidi", "Menu in-game", "Screenshot", "Schermo intero", "Avanzamento rapido",
             "Premi per cambiare questo tasto rapido (Esc annulla)."},
            {"Tastenkürzel", "Spielmenü", "Screenshot", "Vollbild", "Vorspulen",
             "Drücken zum Neubelegen (Esc bricht ab)."},
            {"Raccourcis", "Menu en jeu", "Capture d'écran", "Plein écran", "Avance rapide",
             "Appuyez pour modifier ce raccourci (Échap annule)."},
            {"Atajos", "Menú en juego", "Captura de pantalla", "Pantalla completa", "Avance rápido",
             "Pulsa para reasignar este atajo (Esc cancela)."},
            {"단축키", "게임 내 메뉴", "스크린샷", "전체 화면", "빨리 감기",
             "이 단축키를 다시 지정하려면 누르세요 (Esc 취소)."},
            {"ホットキー", "ゲーム内メニュー", "スクリーンショット", "全画面", "早送り",
             "押してショートカットを変更します（Escでキャンセル）。"},
            {"الاختصارات", "قائمة اللعبة", "لقطة شاشة", "ملء الشاشة", "تسريع",
             "اضغط لإعادة تعيين الاختصار (Esc للإلغاء)."},
            {"Горячие клавиши", "Меню в игре", "Скриншот", "Полный экран", "Ускорение",
             "Нажмите, чтобы переназначить клавишу (Esc — отмена)."},
            {"Kısayollar", "Oyun içi menü", "Ekran görüntüsü", "Tam ekran", "Hızlı ileri sar",
             "Bu kısayolu değiştirmek için basın (Esc iptal eder)."},
            {"Sneltoetsen", "Spelmenu", "Schermafbeelding", "Volledig scherm", "Vooruitspoelen",
             "Druk om deze sneltoets opnieuw toe te wijzen (Esc annuleert)."},
            {"快捷键", "游戏内菜单", "截图", "全屏", "快进",
             "点击以重新绑定此快捷键（Esc 取消）。"},
            {"快捷鍵", "遊戲內選單", "截圖", "全螢幕", "快轉",
             "點擊以重新指定此快捷鍵（Esc 取消）。"},
        };
        rows::section(kHotkeyText[lang][0]);
        for (size_t h = 0; h < kHotkeyCount; ++h) {
            char id[128];
            std::snprintf(id, sizeof id, "%s###hotkey%zu", kHotkeyText[lang][1 + h], h);
            const bool waiting = c.capture_action == int(kControlCount + h);
            const char* name = waiting ? t.press_key : c.host.key_name ? c.host.key_name(s.hotkey_bindings[h]) : "?";
            if (rows::action(id, name, kHotkeyText[lang][5], true, waiting))
                c.capture_action = int(kControlCount + h);
        }
        static constexpr const char* kTitleQuitBtnText[13][2] = {
            {"Quit game (pad)", "Controller button to hold on the title screen to quit the game."},
            {"Esci dal gioco (pad)", "Tasto del controller da tenere premuto nella schermata del titolo per uscire dal gioco."},
            {"Spiel beenden (Pad)", "Controller-Taste, die auf dem Titelbildschirm zum Beenden gedrückt gehalten wird."},
            {"Quitter le jeu (manette)", "Bouton de la manette à maintenir sur l'écran-titre pour quitter le jeu."},
            {"Salir del juego (mando)", "Botón del mando que se debe mantener pulsado en la pantalla de título para salir."},
            {"게임 종료 (패드)", "타이틀 화면에서 길게 눌러 게임을 종료할 컨트롤러 버튼입니다."},
            {"ゲーム終了 (パッド)", "タイトル画面で長押ししてゲームを終了するコントローラーのボタン。"},
            {"إنهاء اللعبة (يد التحكم)", "زر يد التحكم الذي يُضغط مطولاً في شاشة العنوان لإنهاء اللعبة."},
            {"Выход из игры (геймпад)", "Кнопка геймпада для удержания на титульном экране чтобы выйти из игры."},
            {"Oyundan çık (kol)", "Başlık ekranında oyundan çıkmak için basılı tutulacak kumanda düğmesi."},
            {"Spel afsluiten (pad)", "Controllerknop om op het titelscherm ingedrukt te houden om af te sluiten."},
            {"退出游戏（手柄）", "在标题画面按住以退出游戏的手柄按键。"},
            {"結束遊戲（控制器）", "在標題畫面按住以結束遊戲的控制器按鍵。"},
        };
        const char* quit_btns[kControlCount];
        for (size_t i = 0; i < 12; ++i) quit_btns[i] = kActions[i];
        for (size_t i = 0; i < 4; ++i) quit_btns[12 + i] = kInputDirections[lang][i];
        int qbtn = std::clamp(s.title_quit_button, 0, int(kControlCount) - 1);
        if (rows::choice(kTitleQuitBtnText[lang][0], &qbtn, quit_btns, int(kControlCount), kDefaults.title_quit_button,
                         kTitleQuitBtnText[lang][1])) {
            s.title_quit_button = qbtn;
            settings_changed(true);
        }
    }
    rows::section(kExtraUiText[lang].reset_sec);
    {
        char rlabel[64];
        std::snprintf(rlabel, sizeof rlabel, kExtraUiText[lang].reset_tab_fmt, l.controls);
        if (rows::action(rlabel, "", kExtraUiText[lang].reset_tab_desc)) {
            s.keyboard_separate = kDefaults.keyboard_separate;
            s.player_devices = kDefaults.player_devices;
            s.controller_preset = kDefaults.controller_preset;
            s.pad_presets = kDefaults.pad_presets;
            s.button_icons = kDefaults.button_icons;
            s.rumble = kDefaults.rumble;
            s.pad_left_deadzones = kDefaults.pad_left_deadzones;
            s.pad_right_deadzones = kDefaults.pad_right_deadzones;
            s.controller_custom_mask = 0;
            s.pad_custom_mask = {};
            s.controller_bindings = {};
            s.pad_bindings = {};
            for (size_t a = 0; a < kControlCount; ++a)
                if (c.host.default_keyboard_bindings[a]) s.keyboard_bindings[a] = c.host.default_keyboard_bindings[a];
            for (size_t h = 0; h < kHotkeyCount; ++h)
                if (c.host.default_hotkey_bindings[h]) s.hotkey_bindings[h] = c.host.default_hotkey_bindings[h];
            s.title_quit_button = kDefaults.title_quit_button;
            c.capture_action = -1;
            settings_changed(true);
            ui::notify(kExtraUiText[lang].reset_tab_note);
        }
    }
    rows::page_note(slot_preset == 1 ? l.nintendo_desc : l.standard_desc);
    rows::end();
}

void overlay_tab(MenuContext& c) {
    Settings& s = c.settings;
    const int lang = std::clamp(s.language, 0, 12);
    const auto& o = kOverlayLabels[lang];
    rows::begin("##overlay", c.height);
    if (rows::toggle(o[0], &s.metrics_enabled, kDefaults.metrics_enabled, translated_desc(desc::metrics, lang))) settings_changed(true);
    rows::indent(1);
    const struct {
        bool* v;
        bool def;
        const char *label, *desc;
    } items[] = {{&s.metric_fps, kDefaults.metric_fps, o[1], translated_desc(desc::fps, lang)},
                 {&s.metric_one_percent_low, kDefaults.metric_one_percent_low, o[2], translated_desc(desc::low, lang)},
                 {&s.metric_game_speed, kDefaults.metric_game_speed, o[3], translated_desc(desc::speed, lang)},
                 {&s.metric_frametime_graph, kDefaults.metric_frametime_graph, o[4], translated_desc(desc::graph, lang)}};
    for (const auto& it : items) {
        if (!s.metrics_enabled) rows::disable_next();
        if (rows::toggle(it.label, it.v, it.def, it.desc)) settings_changed(true);
    }
    rows::indent(-1);
    if (!s.metrics_enabled) rows::disable_next();
    if (rows::choice(o[5], &s.overlay_position, kPositions[lang], 4, kDefaults.overlay_position, translated_desc(desc::position, lang)))
        settings_changed(true);
    rows::section(kExtraUiText[lang].reset_sec);
    {
        char rlabel[64];
        std::snprintf(rlabel, sizeof rlabel, kExtraUiText[lang].reset_tab_fmt, kLabels[lang].overlay);
        if (rows::action(rlabel, "", kExtraUiText[lang].reset_tab_desc)) {
            s.metrics_enabled = kDefaults.metrics_enabled;
            s.metric_fps = kDefaults.metric_fps;
            s.metric_one_percent_low = kDefaults.metric_one_percent_low;
            s.metric_game_speed = kDefaults.metric_game_speed;
            s.metric_frametime_graph = kDefaults.metric_frametime_graph;
            s.overlay_position = kDefaults.overlay_position;
            settings_changed(true);
            ui::notify(kExtraUiText[lang].reset_tab_note);
        }
    }
    rows::end();
}

void system_tab(MenuContext& c, const Labels& l) {
    Settings& s = c.settings;
    rows::begin("##system", c.height);
    rows::section(l.system);
    const int lang = std::clamp(s.language, 0, 12);
    if (rows::choice(kCommon[lang][11], &s.language, kLanguageNames, int(std::size(kLanguageNames)), kDefaults.language, translated_desc(desc::language, lang)))
        settings_changed(true);
    const char* theme_names[5] = {};
    for (int i = 0; i < ui::theme_count() && i < 5; ++i) theme_names[i] = ui::theme_name(i);
    if (rows::choice(kExtraUiText[lang].style_color, &s.theme, theme_names, ui::theme_count(), kDefaults.theme,
                     kExtraUiText[lang].style_color_desc)) {
        ui::set_theme(s.theme);
        settings_changed(true);
    }
    if (rows::integer(l.memory_limit, &s.memory_limit_gb, 2, 32, 1, kDefaults.memory_limit_gb, "%d GB", translated_desc(desc::memory, lang)))
        settings_changed(true);
    if (rows::toggle(l.low_vram, &s.low_vram_profile, kDefaults.low_vram_profile, translated_desc(desc::low_vram, lang)))
        settings_changed(true), ui::notify(l.restart_low_vram);
    static constexpr const char* kDiscordDesc[13] = {
        "Show Super Mario Bros. Wonder activity on Discord.",
        "Mostra su Discord a cosa stai giocando in Super Mario Bros. Wonder.",
        "Zeigt deine Super Mario Bros. Wonder-Aktivität auf Discord an.",
        "Affiche votre activité Super Mario Bros. Wonder sur Discord.",
        "Muestra tu actividad de Super Mario Bros. Wonder en Discord.",
        "Discord에 Super Mario Bros. Wonder 플레이 상태를 표시합니다.",
        "Discordにスーパーマリオブラザーズ ワンダーのプレイ状況を表示します。",
        "عرض نشاط Super Mario Bros. Wonder على Discord.",
        "Показывать активность Super Mario Bros. Wonder в Discord.",
        "Discord üzerinde Super Mario Bros. Wonder etkinliğini gösterir.",
        "Toont je Super Mario Bros. Wonder-activiteit op Discord.",
        "在 Discord 上显示《超级马力欧兄弟 惊奇》的游玩状态。",
        "在 Discord 上顯示《超級瑪利歐兄弟 驚奇》的遊玩狀態。",
    };
    if (rows::toggle("Discord Rich Presence", &s.discord_rpc, kDefaults.discord_rpc, kDiscordDesc[lang]))
        settings_changed(true);
    {
        constexpr const char* kBgPauseText[13][2] = {
            {"Pause in background", "Automatically pauses the game and audio when the window loses focus or is minimized."},
            {"Pausa in background", "Mette automaticamente in pausa il gioco e l'audio quando la finestra perde il focus o è minimizzata."},
            {"Im Hintergrund pausieren", "Pausiert Spiel und Audio automatisch, wenn das Fenster den Fokus verliert oder minimiert wird."},
            {"Pause en arrière-plan", "Met automatiquement le jeu et le son en pause lorsque la fenêtre perd le focus ou est réduite."},
            {"Pausar en segundo plano", "Pausa automáticamente el juego y el audio cuando la ventana pierde el foco o se minimiza."},
            {"백그라운드 일시정지", "창이 포커스를 잃거나 최소화되면 게임과 오디오를 자동으로 일시정지합니다."},
            {"バックグラウンド時に一時停止", "ウィンドウが非アクティブまたは最小化されると、ゲームと音声を自動で一時停止します。"},
            {"إيقاف مؤقت في الخلفية", "يوقف اللعبة والصوت مؤقتاً تلقائياً عند فقدان النافذة للتركيز أو تصغيرها."},
            {"Пауза в фоне", "Автоматически ставит игру и звук на паузу при потере фокуса или сворачивании окна."},
            {"Arka planda duraklat", "Pencere odağı kaybettiğinde veya simge durumuna küçültüldüğünde oyunu ve sesi otomatik duraklatır."},
            {"Pauzeren op achtergrond", "Pauzeert het spel en geluid automatisch wanneer het venster de focus verliest of wordt geminimaliseerd."},
            {"后台暂停", "窗口失去焦点或最小化时自动暂停游戏和音频。"},
            {"背景暫停", "視窗失去焦點或最小化時自動暫停遊戲與音訊。"},
        };
        if (rows::toggle(kBgPauseText[lang][0], &s.pause_in_background, kDefaults.pause_in_background, kBgPauseText[lang][1]))
            settings_changed(true);
    }
    {
        constexpr const char* kBgPadText[13][2] = {
            {"Ignore controller in background", "Ignores controller input while the game window is out of focus."},
            {"Ignora controller in background", "Ignora gli input del controller quando la finestra di gioco non è in primo piano."},
            {"Controller im Hintergrund ignorieren", "Ignoriert Controller-Eingaben, wenn das Spielfenster keinen Fokus hat."},
            {"Ignorer la manette en arrière-plan", "Ignore les entrées de la manette lorsque la fenêtre du jeu n'est pas active."},
            {"Ignorar mando en segundo plano", "Ignora las entradas del mando cuando la ventana del juego pierde el foco."},
            {"백그라운드에서 컨트롤러 무시", "게임 창이 포커스를 잃으면 컨트롤러 입력을 무시합니다."},
            {"バックグラウンド時にコントローラーを無効化", "ゲームウィンドウが非アクティブの間、コントローラー入力を無視します。"},
            {"تجاهل وحدة التحكم في الخلفية", "يتجاهل إدخال وحدة التحكم عندما تفقد نافذة اللعبة التركيز."},
            {"Игнорировать геймпад в фоне", "Игнорирует ввод с геймпада, когда окно игры не в фокусе."},
            {"Arka planda kontrolcüyü yoksay", "Oyun penceresi odak dışındayken kontrolcü girişini yoksayar."},
            {"Controller op achtergrond negeren", "Negeert controllerinvoer zolang het spelvenster geen focus heeft."},
            {"后台忽略手柄输入", "游戏窗口失去焦点时忽略手柄输入。"},
            {"背景忽略控制器輸入", "遊戲視窗失去焦點時忽略控制器輸入。"},
        };
        if (rows::toggle(kBgPadText[lang][0], &s.ignore_pad_in_background, kDefaults.ignore_pad_in_background, kBgPadText[lang][1]))
            settings_changed(true);
    }
    {
        constexpr const char* kFolderActionsText[13][4] = {
            {"Open data folder", "Opens the active data folder (saves, nx.log, mods) in your file manager.",
             "Open screenshots folder", "Opens the folder where F2 PNG screenshots are saved."},
            {"Apri cartella dati", "Apre la cartella dati attiva (salvataggi, log nx.log, mod).",
             "Apri cartella screenshot", "Apre la cartella dove vengono salvati gli screenshot PNG (F2)."},
            {"Datenordner öffnen", "Öffnet den aktiven Datenordner (Spielstände, nx.log, Mods) im Dateimanager.",
             "Screenshot-Ordner öffnen", "Öffnet den Ordner, in dem F2-PNG-Screenshots gespeichert werden."},
            {"Ouvrir le dossier de données", "Ouvre le dossier de données actif (sauvegardes, nx.log, mods).",
             "Ouvrir le dossier des captures", "Ouvre le dossier où sont enregistrées les captures PNG (F2)."},
            {"Abrir carpeta de datos", "Abre la carpeta de datos activa (partidas, nx.log, mods).",
             "Abrir carpeta de capturas", "Abre la carpeta donde se guardan las capturas PNG (F2)."},
            {"데이터 폴더 열기", "활성 데이터 폴더(저장 데이터, nx.log, 모드)를 엽니다.",
             "스크린샷 폴더 열기", "F2 PNG 스크린샷이 저장되는 폴더를 엽니다."},
            {"データフォルダーを開く", "現在のデータフォルダー（セーブ、nx.log、MOD）を開きます。",
             "スクリーンショットフォルダーを開く", "F2で撮影したPNGスクリーンショットの保存先を開きます。"},
            {"فتح مجلد البيانات", "يفتح مجلد البيانات النشط (ملفات الحفظ، nx.log، التعديلات).",
             "فتح مجلد لقطات الشاشة", "يفتح المجلد الذي تُحفظ فيه لقطات شاشة PNG (F2)."},
            {"Открыть папку данных", "Открывает активную папку данных (сохранения, nx.log, моды).",
             "Открыть папку скриншотов", "Открывает папку, куда сохраняются PNG-скриншоты (F2)."},
            {"Veri klasörünü aç", "Etkin veri klasörünü (kayıtlar, nx.log, modlar) açar.",
             "Ekran görüntüsü klasörünü aç", "F2 PNG ekran görüntülerinin kaydedildiği klasörü açar."},
            {"Gegevensmap openen", "Opent de actieve gegevensmap (opslag, nx.log, mods).",
             "Schermafbeeldingenmap openen", "Opent de map waarin F2 PNG-schermafbeeldingen worden opgeslagen."},
            {"打开数据文件夹", "在文件管理器中打开当前数据文件夹（存档、nx.log、Mod）。",
             "打开截图文件夹", "打开保存 F2 PNG 截图的文件夹。"},
            {"開啟資料夾", "在檔案總管中開啟目前資料夾（存檔、nx.log、Mod）。",
             "開啟截圖資料夾", "開啟儲存 F2 PNG 截圖的資料夾。"},
        };
        const char* open_data_label = kFolderActionsText[lang][0];
        const char* open_data_desc = kFolderActionsText[lang][1];
        if (rows::action(open_data_label, "", open_data_desc)) plat::open_folder(plat::data_root());
        const char* open_shots_label = kFolderActionsText[lang][2];
        const char* open_shots_desc = kFolderActionsText[lang][3];
        if (rows::action(open_shots_label, "", open_shots_desc)) {
            const std::string dir = plat::data_root() + "/screenshots";
            plat::make_dirs(dir);
            plat::open_folder(dir);
        }
    }
    rows::section(kExtraUiText[lang].reset_sec);
    {
        char rlabel[64];
        std::snprintf(rlabel, sizeof rlabel, kExtraUiText[lang].reset_tab_fmt, l.system);
        if (rows::action(rlabel, "", kExtraUiText[lang].reset_tab_desc)) {
            s.language = kDefaults.language, s.theme = kDefaults.theme;
            s.memory_limit_gb = kDefaults.memory_limit_gb, s.low_vram_profile = kDefaults.low_vram_profile;
            s.discord_rpc = kDefaults.discord_rpc, s.pause_in_background = kDefaults.pause_in_background;
            s.ignore_pad_in_background = kDefaults.ignore_pad_in_background;
            ui::set_theme(s.theme);
            settings_changed(true);
            ui::notify(kExtraUiText[lang].reset_tab_note);
        }
    }
    static double reset_all_at = -10;
    const bool armed_all = ImGui::GetTime() - reset_all_at < 4.0;
    if (rows::action(kExtraUiText[lang].reset_all,
                     armed_all ? kExtraUiText[lang].reset_all_confirm : "",
                     kExtraUiText[lang].reset_all_desc,
                     true, armed_all)) {
        if (armed_all) {
            Settings fresh = kDefaults;
            for (size_t a = 0; a < kControlCount; ++a)
                fresh.keyboard_bindings[a] = c.host.default_keyboard_bindings[a] ? c.host.default_keyboard_bindings[a] : s.keyboard_bindings[a];
            for (size_t h = 0; h < kHotkeyCount; ++h)
                fresh.hotkey_bindings[h] = c.host.default_hotkey_bindings[h] ? c.host.default_hotkey_bindings[h] : s.hotkey_bindings[h];
            s = fresh;
            c.capture_action = -1;
            ui::set_theme(s.theme);
            settings_changed(true);
            reset_all_at = -10;
            ui::notify(kExtraUiText[lang].reset_all_note);
        } else {
            reset_all_at = ImGui::GetTime();
        }
    }
    // Quit: A once arms it (the row pulses, "Press again to quit"), A again within 4 s quits as closing the window does
    // (the pipeline cache is saved); progress since the game's last save is lost
    static double armed_at = -10;
    const bool armed = ImGui::GetTime() - armed_at < 4.0;
    rows::section(kCommon[lang][9]);
    if (rows::action(kCommon[lang][10], armed ? "Press again to quit" : "", translated_desc(desc::quit, lang), true, armed)) {
        if (armed) log("overlay: quit from the menu"), plat::exit_process(0);
        armed_at = ImGui::GetTime();
    }
    rows::page_note(l.restart_low_vram);
    rows::end();
}

void network_tab(MenuContext& c) {
    rows::begin("##network", c.height);
    const nextendo::Status nx_st = nextendo::status();
    bool nx_on = nx_st.enabled;
    rows::section("Nextendo Network");
    if (rows::toggle("Nextendo Online", &nx_on, false,
                     "Enable online multiplayer via Nextendo Network (requires sign-in and Wonder 1.2.1 NPLN patch).")) {
        nextendo::set_enabled(nx_on);
        if (nx_on && c.settings.speedrun_timer) {
            c.settings.speedrun_timer = false;
            settings_changed(true);
            ui::notify("Nextendo Online enabled (Speedrun options disabled).");
        } else {
            ui::notify(nx_on ? "Nextendo Online enabled (restart to apply)." : "Nextendo Online disabled.");
        }
    }
    std::string acc_val = nx_st.login_in_progress
        ? "Signing in (check browser)..."
        : (nx_st.signed_in
               ? (nx_st.username.empty() ? ("PID " + std::to_string(nx_st.pid))
                                         : (nx_st.username + " (" + nx_st.friend_code + ")"))
               : (nx_st.last_error.empty() ? "Not signed in" : nx_st.last_error));
    if (rows::action(nx_st.signed_in ? "Nextendo Sign Out" : "Sign in with Nextendo", acc_val.c_str(),
                     "Authenticate your Nextendo account via OAuth 2.0 PKCE in your browser.",
                     !nx_st.login_in_progress, nx_st.signed_in)) {
        if (nx_st.signed_in) {
            nextendo::sign_out();
            ui::notify("Signed out from Nextendo.");
        } else {
            nextendo::begin_oauth_login();
            ui::notify("Browser opened for Nextendo OAuth login.");
        }
    }
    rows::end();
}

struct SpeedrunLabels {
    const char *section, *timer_toggle, *timer_desc, *mode_label, *mode_desc, *pos_label, *pos_desc, *size_label,
        *size_desc, *color_label, *color_desc, *online_warning;
};

constexpr SpeedrunLabels kSpeedrunLabels[13] = {
    {"Course Timer", "Course timer",
     "Automatically starts a Wonder-styled timer when you enter a course and resets it on exit or restart.",
     "Timer mode", "Choose In-Game Time (IGT, pauses in menu), Real Time (RTA), or both.",
     "Timer position", "Where on the screen the course timer is shown.",
     "Timer size", "Adjust the scale of the on-screen course timer.",
     "Timer color", "Choose the color of the timer numerals.",
     "Nextendo Online is active. Disable Nextendo Online in the Network tab to enable Speedrun options."},
    {"Timer Livello", "Timer livello",
     "Avvia automaticamente un timer in stile Wonder quando entri in un livello e lo azzera all'uscita.",
     "Modalità timer", "Scegli tra Tempo di gioco (IGT, si ferma nel menu), Tempo reale (RTA) o entrambi.",
     "Posizione timer", "Posizione sullo schermo in cui mostrare il timer del livello.",
     "Grandezza timer", "Regola la dimensione del timer su schermo.",
     "Colore timer", "Scegli il colore delle cifre del timer.",
     "Nextendo Online è attivo. Disattiva Nextendo Online nella scheda Network per attivare le opzioni Speedrun."},
    {"Level-Timer", "Level-Timer",
     "Startet beim Betreten eines Levels automatisch einen Timer im Wonder-Stil und setzt ihn beim Verlassen zurück.",
     "Timer-Modus", "Wähle Spielzeit (IGT, pausiert im Menü), Echtzeit (RTA) oder beides.",
     "Timer-Position", "Position auf dem Bildschirm, an der der Level-Timer angezeigt wird.",
     "Timer-Größe", "Skalierung des Level-Timers anpassen.",
     "Timer-Farbe", "Farbe der Timer-Ziffern auswählen.",
     "Nextendo Online ist aktiv. Deaktiviere Nextendo Online im Tab Network, um Speedrun-Optionen zu aktivieren."},
    {"Chronomètre de niveau", "Chronomètre de niveau",
     "Démarre automatiquement un chronomètre style Wonder à l'entrée d'un niveau et le réinitialise à la sortie.",
     "Mode du chronomètre", "Choisissez Temps en jeu (IGT, en pause dans le menu), Temps réel (RTA) ou les deux.",
     "Position du chronomètre", "Emplacement à l'écran où le chronomètre est affiché.",
     "Taille du chronomètre", "Ajuster la taille du chronomètre à l'écran.",
     "Couleur du chronomètre", "Choisir la couleur des chiffres du chronomètre.",
     "Nextendo Online est actif. Désactivez Nextendo Online dans l'onglet Network pour activer les options Speedrun."},
    {"Cronómetro de nivel", "Cronómetro de nivel",
     "Inicia automáticamente un cronómetro estilo Wonder al entrar en un nivel y lo reinicia al salir.",
     "Modo del cronómetro", "Elige Tiempo de juego (IGT, se pausa en el menú), Tiempo real (RTA) o ambos.",
     "Posición del cronómetro", "Ubicación en pantalla donde se muestra el cronómetro.",
     "Tamaño del cronómetro", "Ajusta el tamaño del cronómetro en pantalla.",
     "Color del cronómetro", "Elige el color de los números del cronómetro.",
     "Nextendo Online está activo. Desactiva Nextendo Online en la pestaña Network para activar las opciones Speedrun."},
    {"코스 타이머", "코스 타이머",
     "코스에 입장하면 Wonder 스타일 타이머가 자동으로 시작되고 퇴장 시 초기화됩니다.",
     "타이머 모드", "게임 내 시간(IGT, 메뉴에서 일시정지), 실제 시간(RTA) 또는 둘 다 선택합니다.",
     "타이머 위치", "코스 타이머를 표시할 화면 위치입니다.",
     "타이머 크기", "화면에 표시되는 코스 타이머의 크기를 조절합니다.",
     "타이머 색상", "타이머 숫자의 색상을 선택합니다.",
     "Nextendo Online이 활성화되어 있습니다. Speedrun 옵션을 켜려면 Network 탭에서 Nextendo Online을 비활성화하세요."},
    {"コースタイマー", "コースタイマー",
     "コースに入るとWonder風タイマーが自動で開始し、退出時にリセットされます。",
     "タイマーモード", "ゲーム内時間(IGT・メニューで停止)、実時間(RTA)、または両方を選択します。",
     "タイマー位置", "コースタイマーを表示する画面上の位置を選択します。",
     "タイマーサイズ", "画面上のコースタイマーの大きさを調整します。",
     "タイマーの色", "タイマーの数字の色を選択します。",
     "Nextendo Onlineが有効です。Speedrunオプションを有効にするにはNetworkタブでNextendo Onlineを無効にしてください。"},
    {"مؤقت المرحلة", "مؤقت المرحلة",
     "يبدأ مؤقت بنمط Wonder تلقائيا عند دخول المرحلة ويعاد ضبطه عند الخروج.",
     "وضع المؤقت", "اختر وقت اللعب (IGT يتوقف في القائمة) أو الوقت الحقيقي (RTA) أو كليهما.",
     "موضع المؤقت", "موضع ظهور مؤقت المرحلة على الشاشة.",
     "حجم المؤقت", "اضبط حجم مؤقت المرحلة على الشاشة.",
     "لون المؤقت", "اختر لون أرقام المؤقت.",
     "Nextendo Online نشط. عطل Nextendo Online من تبويب Network لتفعيل خيارات Speedrun."},
    {"Таймер уровня", "Таймер уровня",
     "Автоматически запускает таймер в стиле Wonder при входе на уровень и сбрасывает его при выходе.",
     "Режим таймера", "Выберите игровое время (IGT, пауза в меню), реальное время (RTA) или оба варианта.",
     "Положение таймера", "Место на экране, где отображается таймер уровня.",
     "Размер таймера", "Настройка масштаба таймера на экране.",
     "Цвет таймера", "Выберите цвет цифр таймера.",
     "Nextendo Online активен. Отключите Nextendo Online на вкладке Network, чтобы включить параметры Speedrun."},
    {"Bölüm Zamanlayıcı", "Bölüm zamanlayıcı",
     "Bir bölüme girdiğinizde Wonder tarzı zamanlayıcıyı otomatik başlatır ve çıkışta sıfırlar.",
     "Zamanlayıcı modu", "Oyun İçi Süre (IGT, menüde durur), Gerçek Süre (RTA) veya ikisini birden seçin.",
     "Zamanlayıcı konumu", "Bölüm zamanlayıcısının ekranda gösterileceği konum.",
     "Zamanlayıcı boyutu", "Ekrandaki bölüm zamanlayıcısının boyutunu ayarlayın.",
     "Zamanlayıcı rengi", "Zamanlayıcı rakamlarının rengini seçin.",
     "Nextendo Online etkin. Speedrun seçeneklerini açmak için Network sekmesinden Nextendo Online'ı devre dışı bırakın."},
    {"Leveltimer", "Leveltimer",
     "Start automatisch een timer in Wonder-stijl bij het betreden van een level en reset bij verlaten.",
     "Timermodus", "Kies speeltijd (IGT, pauzeert in menu), echte tijd (RTA) of beide.",
     "Timerpositie", "Plaats op het scherm waar de leveltimer wordt getoond.",
     "Timergrootte", "Pas de grootte van de leveltimer op het scherm aan.",
     "Timerkleur", "Kies de kleur van de timercijfers.",
     "Nextendo Online is actief. Schakel Nextendo Online uit op het tabblad Network om Speedrun-opties in te schakelen."},
    {"关卡计时器", "关卡计时器",
     "进入关卡时自动启动 Wonder 风格计时器，退出关卡时重置。",
     "计时器模式", "选择游戏内时间 (IGT，菜单中暂停)、真实时间 (RTA) 或同时显示。",
     "计时器位置", "在屏幕上显示关卡计时器的位置。",
     "计时器大小", "调整屏幕上关卡计时器的显示大小。",
     "计时器颜色", "选择计时器数字的颜色。",
     "Nextendo Online 已启用。请在 Network 选项卡中禁用 Nextendo Online 以启用 Speedrun 选项。"},
    {"關卡計時器", "關卡計時器",
     "進入關卡時自動啟動 Wonder 風格計時器，離開關卡時重設。",
     "計時器模式", "選擇遊戲內時間 (IGT，選單中暫停)、真實時間 (RTA) 或同時顯示。",
     "計時器位置", "在螢幕上顯示關卡計時器的位置。",
     "計時器大小", "調整螢幕上關卡計時器的顯示大小。",
     "計時器顏色", "選擇計時器數字的顏色。",
     "Nextendo Online 已啟用。請在 Network 分頁中停用 Nextendo Online 以啟用 Speedrun 選項。"},
};

constexpr const char* kSpeedrunPositions[13][6] = {
    {"Top left", "Top center", "Top right", "Bottom left", "Bottom center", "Bottom right"},
    {"In alto a sinistra", "In alto al centro", "In alto a destra", "In basso a sinistra", "In basso al centro", "In basso a destra"},
    {"Oben links", "Oben Mitte", "Oben rechts", "Unten links", "Unten Mitte", "Unten rechts"},
    {"En haut à gauche", "En haut au centre", "En haut à droite", "En bas à gauche", "En bas au centre", "En bas à droite"},
    {"Arriba izquierda", "Arriba centro", "Arriba derecha", "Abajo izquierda", "Abajo centro", "Abajo derecha"},
    {"좌측 상단", "중앙 상단", "우측 상단", "좌측 하단", "중앙 하단", "우측 하단"},
    {"左上", "中央上", "右上", "左下", "中央下", "右下"},
    {"أعلى اليسار", "أعلى الوسط", "أعلى اليمين", "أسفل اليسار", "أسفل الوسط", "أسفل اليمين"},
    {"Сверху слева", "Сверху по центру", "Сверху справа", "Снизу слева", "Снизу по центру", "Снизу справа"},
    {"Sol üst", "Orta üst", "Sağ üst", "Sol alt", "Orta alt", "Sağ alt"},
    {"Linksboven", "Middenboven", "Rechtsboven", "Linksonder", "Middenonder", "Rechtsonder"},
    {"左上", "中上", "右上", "左下", "中下", "右下"},
    {"左上", "中上", "右上", "左下", "中下", "右下"},
};

constexpr const char* kSpeedrunSizes[13][4] = {
    {"Small", "Normal", "Large", "Extra large"},
    {"Piccolo", "Normale", "Grande", "Molto grande"},
    {"Klein", "Normal", "Groß", "Sehr groß"},
    {"Petit", "Normal", "Grand", "Très grand"},
    {"Pequeño", "Normal", "Grande", "Muy grande"},
    {"작게", "보통", "크게", "매우 크게"},
    {"小", "標準", "大", "特大"},
    {"صغير", "عادي", "كبير", "كبير جدا"},
    {"Маленький", "Обычный", "Большой", "Очень большой"},
    {"Küçük", "Normal", "Büyük", "Çok büyük"},
    {"Klein", "Normaal", "Groot", "Extra groot"},
    {"小", "标准", "大", "超大"},
    {"小", "標準", "大", "特大"},
};

constexpr const char* kSpeedrunColors[13][6] = {
    {"White", "Gold", "Cyan", "Green", "Red", "Pink"},
    {"Bianco", "Oro", "Ciano", "Verde", "Rosso", "Rosa"},
    {"Weiß", "Gold", "Cyan", "Grün", "Rot", "Rosa"},
    {"Blanc", "Or", "Cyan", "Vert", "Rouge", "Rose"},
    {"Blanco", "Oro", "Cian", "Verde", "Rojo", "Rosa"},
    {"흰색", "금색", "청록", "초록", "빨강", "분홍"},
    {"白", "ゴールド", "シアン", "緑", "赤", "ピンク"},
    {"أبيض", "ذهبي", "سماوي", "أخضر", "أحمر", "وردي"},
    {"Белый", "Золотой", "Голубой", "Зелёный", "Красный", "Розовый"},
    {"Beyaz", "Altın", "Camgöbeği", "Yeşil", "Kırmızı", "Pembe"},
    {"Wit", "Goud", "Cyaan", "Groen", "Rood", "Roze"},
    {"白色", "金色", "青色", "绿色", "红色", "粉色"},
    {"白色", "金色", "青色", "綠色", "紅色", "粉色"},
};

constexpr const char* kSpeedrunModes[13][3] = {
    {"IGT", "RTA", "IGT + RTA"},
    {"IGT", "RTA", "IGT + RTA"},
    {"IGT", "RTA", "IGT + RTA"},
    {"IGT", "RTA", "IGT + RTA"},
    {"IGT", "RTA", "IGT + RTA"},
    {"IGT", "RTA", "IGT + RTA"},
    {"IGT", "RTA", "IGT + RTA"},
    {"IGT", "RTA", "IGT + RTA"},
    {"IGT", "RTA", "IGT + RTA"},
    {"IGT", "RTA", "IGT + RTA"},
    {"IGT", "RTA", "IGT + RTA"},
    {"IGT", "RTA", "IGT + RTA"},
    {"IGT", "RTA", "IGT + RTA"},
};

void speedrun_tab(MenuContext& c) {
    Settings& s = c.settings;
    const int lang = std::clamp(s.language, 0, 12);
    const SpeedrunLabels& sl = kSpeedrunLabels[lang];
    const bool online_on = nextendo::status().enabled;
    rows::begin("##speedrun", c.height);
    if (online_on) rows::page_note(sl.online_warning);
    rows::section(sl.section);
    bool timer_val = online_on ? false : s.speedrun_timer;
    if (online_on) rows::disable_next();
    if (rows::toggle(sl.timer_toggle, &timer_val, false, online_on ? sl.online_warning : sl.timer_desc) && !online_on) {
        s.speedrun_timer = timer_val;
        settings_changed(true);
    }
    if (online_on || !s.speedrun_timer) rows::disable_next();
    if (rows::choice(sl.mode_label, &s.speedrun_timer_mode, kSpeedrunModes[lang], 3, 0,
                     online_on ? sl.online_warning : sl.mode_desc) &&
        !online_on)
        settings_changed(true);
    if (online_on || !s.speedrun_timer) rows::disable_next();
    if (rows::choice(sl.pos_label, &s.speedrun_timer_position, kSpeedrunPositions[lang], 6, 2,
                     online_on ? sl.online_warning : sl.pos_desc) &&
        !online_on)
        settings_changed(true);
    if (online_on || !s.speedrun_timer) rows::disable_next();
    if (rows::choice(sl.size_label, &s.speedrun_timer_size, kSpeedrunSizes[lang], 4, 1,
                     online_on ? sl.online_warning : sl.size_desc) &&
        !online_on)
        settings_changed(true);
    if (online_on || !s.speedrun_timer) rows::disable_next();
    if (rows::choice(sl.color_label, &s.speedrun_timer_color, kSpeedrunColors[lang], 6, 0,
                     online_on ? sl.online_warning : sl.color_desc) &&
        !online_on)
        settings_changed(true);
    rows::end();
}

struct DebugLabels {
    const char* tab;
    const char* sec_input;
    const char* record_label;
    const char* record_desc;
    const char* sec_diag;
    const char* trace_label;
    const char* trace_desc;
    const char* note;
};

constexpr DebugLabels kDebugLabels[13] = {
    // 0: English
    {"Debug",
     "Input & Replay",
     "Record controller inputs",
     "Records all button presses and stick directions to <data folder>/input_record.txt (same as NX_INPUT_RECORD=1) for automated playback via NX_INPUT_SCRIPT. Toggling off flushes and closes the file.",
     "Diagnostics",
     "Verbose trace log (NX_TRACE)",
     "Writes detailed filesystem, IPC, SVC, and HLE call traces to nx.log in real time (same as NX_TRACE=1). Useful for diagnosing hangs or bugs, with some performance overhead.",
     "Debug toggles apply immediately. Input recording and trace toggles are session-only and reset on restart."},
    // 1: Italiano
    {"Debug",
     "Input e Replay",
     "Registra input controller",
     "Registra in tempo reale tutti i tasti e le direzioni dello stick in <cartella dati>/input_record.txt (equivalente a NX_INPUT_RECORD=1) per il replay con NX_INPUT_SCRIPT. Disattivando salva e chiude il file.",
     "Diagnostica",
     "Log trace dettagliato (NX_TRACE)",
     "Scrive in tempo reale i trace dettagliati di filesystem, IPC, SVC e HLE in nx.log (equivalente a NX_TRACE=1). Utile per diagnosticare blocchi o bug, con un leggero impatto sulle prestazioni.",
     "Le opzioni di debug si applicano subito. Registrazione input e log trace valgono solo per la sessione corrente."},
    // 2: Deutsch
    {"Debug",
     "Eingabe & Replay",
     "Controller-Eingaben aufzeichnen",
     "Zeichnet alle Tastendrucke und Stick-Richtungen in Echtzeit in <Datenordner>/input_record.txt auf (wie NX_INPUT_RECORD=1) fur die Wiedergabe mit NX_INPUT_SCRIPT. Ausschalten speichert und schliesst die Datei.",
     "Diagnose",
     "Ausfuhrliches Trace-Log (NX_TRACE)",
     "Schreibt detaillierte Dateisystem-, IPC-, SVC- und HLE-Aufrufe in Echtzeit in nx.log (wie NX_TRACE=1). Hilfreich bei Fehlern oder Hangern, verursacht etwas Overhead.",
     "Debug-Schalter wirken sofort. Eingabeaufzeichnung und Trace-Log gelten nur fur diese Sitzung."},
    // 3: Français
    {"Debug",
     "Entrees et Replay",
     "Enregistrer les entrees manette",
     "Enregistre en temps reel tous les boutons et directions du stick dans <dossier donnees>/input_record.txt (comme NX_INPUT_RECORD=1) pour la lecture via NX_INPUT_SCRIPT. Desactiver enregistre et ferme le fichier.",
     "Diagnostics",
     "Journal trace detaille (NX_TRACE)",
     "Ecrit les traces detaillees du systeme de fichiers, IPC, SVC et HLE dans nx.log en temps reel (comme NX_TRACE=1). Utile pour diagnostiquer les bugs, avec un leger cout en performances.",
     "Les options de debug s'appliquent immediatement. L'enregistrement et les traces sont limites a la session."},
    // 4: Español
    {"Debug",
     "Entrada y Replay",
     "Grabar entradas del mando",
     "Graba en tiempo real todos los botones y direcciones del stick en <carpeta de datos>/input_record.txt (igual que NX_INPUT_RECORD=1) para reproducirlo con NX_INPUT_SCRIPT. Desactivar guarda y cierra el archivo.",
     "Diagnostico",
     "Registro trace detallado (NX_TRACE)",
     "Escribe trazas detalladas de archivos, IPC, SVC y HLE en nx.log en tiempo real (igual que NX_TRACE=1). Util para diagnosticar bloqueos o errores, con algo de sobrecarga.",
     "Las opciones de depuracion se aplican al instante. La grabacion y el registro solo duran esta sesion."},
    // 5: 한국어
    {"디버그",
     "입력 및 리플레이",
     "컨트롤러 입력 기록",
     "모든 버튼 입력과 스틱 방향을 <데이터 폴더>/input_record.txt에 실시간으로 기록합니다(NX_INPUT_RECORD=1과 동일). 끄면 파일이 저장되고 닫힙니다.",
     "진단",
     "상세 추적 로그 (NX_TRACE)",
     "파일 시스템, IPC, SVC, HLE 호출 추적을 nx.log에 실시간으로 기록합니다(NX_TRACE=1과 동일). 오류 진단에 유용하지만 약간의 성능 부하가 있습니다.",
     "디버그 설정은 즉시 적용됩니다. 입력 기록과 추적 로그는 현재 세션에만 유지됩니다."},
    // 6: 日本語
    {"デバッグ",
     "入力とリプレイ",
     "コントローラー入力を記録",
     "すべてのボタン入力とスティック方向を <データフォルダ>/input_record.txt にリアルタイム記録します（NX_INPUT_RECORD=1 と同等）。オフにすると保存して閉じます。",
     "診断",
     "詳細トレースログ (NX_TRACE)",
     "ファイルシステム、IPC、SVC、HLE 呼び出しの詳細トレースを nx.log にリアルタイム出力します（NX_TRACE=1 と同等）。不具合調査に有用ですが少し負荷がかかります。",
     "デバッグ設定は即座に反映されます。入力記録とトレースログはこのセッション限り有効です。"},
    // 7: Русский
    {"Отладка",
     "Ввод и повтор",
     "Запись ввода геймпада",
     "Записывает все нажатия кнопок и направления стика в <папка данных>/input_record.txt в реальном времени (как NX_INPUT_RECORD=1). Отключение сохраняет и закрывает файл.",
     "Диагностика",
     "Подробный лог трассировки (NX_TRACE)",
     "Записывает подробную трассировку ФС, IPC, SVC и HLE в nx.log в реальном времени (как NX_TRACE=1). Полезно для поиска зависаний и ошибок, но немного снижает производительность.",
     "Параметры отладки применяются сразу. Запись ввода и трассировка сбрасываются после перезапуска."},
    // 8: العربية
    {"تصحيح",
     "الإدخال والإعادة",
     "تسجيل إدخال وحدة التحكم",
     "يسجل جميع الأزرار واتجاهات العصا في <مجلد البيانات>/input_record.txt في الوقت الفعلي (مثل NX_INPUT_RECORD=1). إيقاف التشغيل يحفظ الملف ويغلقه.",
     "التشخيص",
     "سجل تتبع مفصل (NX_TRACE)",
     "يكتب تتبعات نظام الملفات وIPC وSVC وHLE في nx.log في الوقت الفعلي (مثل NX_TRACE=1). مفيد لتشخيص الأعطال مع بعض الحمل على الأداء.",
     "تطبق خيارات التصحيح فورا. التسجيل وسجل التتبع يخصان الجلسة الحالية فقط."},
    // 9: Türkçe
    {"Hata Ayıklama",
     "Girdi ve Tekrar",
     "Kontrolcü girdilerini kaydet",
     "Tüm düğme basışlarını ve çubuk yönlerini <veri klasörü>/input_record.txt dosyasına gerçek zamanlı kaydeder (NX_INPUT_RECORD=1 ile aynı). Kapatmak dosyayı kaydedip kapatır.",
     "Tanılama",
     "Ayrıntılı izleme günlüğü (NX_TRACE)",
     "Dosya sistemi, IPC, SVC ve HLE çağrılarını gerçek zamanlı olarak nx.log dosyasına yazar (NX_TRACE=1 ile aynı). Hataları bulmak için faydalıdır, performansı bir miktar etkileyebilir.",
     "Hata ayıklama seçenekleri anında uygulanır. Girdi kaydı ve izleme günlüğü yalnızca bu oturum içindir."},
    // 10: Nederlands
    {"Debug",
     "Invoer & Replay",
     "Controller-invoer opnemen",
     "Neemt alle knoppen en stickrichtingen live op naar <datamap>/input_record.txt (hetzelfde als NX_INPUT_RECORD=1) voor herhaling via NX_INPUT_SCRIPT. Uitschakelen slaat het bestand op en sluit het.",
     "Diagnose",
     "Uitgebreide tracelog (NX_TRACE)",
     "Schrijft gedetailleerde bestandssysteem-, IPC-, SVC- en HLE-aanroepen live naar nx.log (hetzelfde als NX_TRACE=1). Handig bij foutopsporing, met lichte prestatie-impact.",
     "Debug-opties werken direct. Invoeropname en tracelog gelden alleen voor de huidige sessie."},
    // 11: 简体中文
    {"调试",
     "输入与回放",
     "录制手柄输入",
     "实时将所有按键与摇杆方向记录到 <数据目录>/input_record.txt（等同于 NX_INPUT_RECORD=1），供 NX_INPUT_SCRIPT 回放使用。关闭后会保存并关闭文件。",
     "诊断",
     "详细跟踪日志 (NX_TRACE)",
     "实时将文件系统、IPC、SVC 和 HLE 调用跟踪写入 nx.log（等同于 NX_TRACE=1）。有助于排查卡死或错误，会带来一定性能开销。",
     "调试开关立即生效。输入录制与跟踪日志仅在当前会话有效，重启后重置。"},
    // 12: 繁體中文
    {"除錯",
     "輸入與重播",
     "錄製手把輸入",
     "即時將所有按鍵與搖桿方向記錄到 <資料夾>/input_record.txt（等同於 NX_INPUT_RECORD=1），供 NX_INPUT_SCRIPT 重播使用。關閉後會儲存並關閉檔案。",
     "診斷",
     "詳細追蹤日誌 (NX_TRACE)",
     "即時將檔案系統、IPC、SVC 與 HLE 呼叫追蹤寫入 nx.log（等同於 NX_TRACE=1）。有助於排查卡頓或錯誤，會帶來些許效能開銷。",
     "除錯開關立即生效。輸入錄製與追蹤日誌僅於目前工作階段有效，重啟後重置。"},
};

void debug_tab(MenuContext& c) {
    Settings& s = c.settings;
    const int lang = std::clamp(s.language, 0, 12);
    const DebugLabels& dl = kDebugLabels[lang];
    rows::begin("debug", c.height);

    rows::section(dl.sec_input);
    bool rec = plat::input_recording();
    if (rows::toggle(dl.record_label, &rec, false, dl.record_desc)) plat::set_input_recording(rec);

    rows::section(dl.sec_diag);
    rows::toggle(dl.trace_label, &nx::g_trace, false, dl.trace_desc);

    rows::page_note(dl.note);
    rows::end();
}

// The close X in the top-right corner (as Savage Wind's menu.mm: lit when hovered or selected).
bool close_button(float k) {
    const float bx = 44 * k;
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    const bool hot = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible);
    const float t = ui::eased(ImGui::GetItemID(), hot ? 1.0f : 0.0f, 22.0f);
    const ImU32 col = ui::fade(ui::mix(ui::theme().text_dim, ui::theme().accent, t));
    const float m = bx * 0.3f, w = (2.4f + 0.6f * t) * 1.5f * k;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), ui::fade(IM_COL32(255, 255, 255, 20), t), 8 * k);
    dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, w);
    dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, w);
    return close;
}

}  // namespace

int menu_tab_count() { return kTabs; }
const char* const* menu_language_names() { return kLanguageNames; }

void draw_menu(MenuContext& c) {
    Settings& s = c.settings;
    ui::set_theme(s.theme);
    const Labels& l = kLabels[std::clamp(s.language, 0, 12)];
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float k = std::clamp(c.height / 1080.0f, 0.5f, 3.0f);
    // Modern minimalist window: up to 1480 px wide at 1080 p, rising subtly as it fades in.
    const ImVec2 size{std::min(display.x * 0.90f, 1480 * k), display.y - 2 * 28 * k};
    const float rise = (1.0f - ui::smoothstep(c.reveal)) * 20 * k;
    ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f + rise}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (c.appearing) ImGui::SetNextWindowFocus();  // keyboard and controller navigation start in the menu
    if (!ImGui::Begin("##nx_menu", nullptr, f)) {
        ImGui::End();
        return;
    }
    ImDrawList* wdl = ImGui::GetWindowDrawList();
    ImGui::PushFont(ui::title());
    ImGui::SetWindowFontScale(0.70f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::theme().text));
    ImGui::TextUnformatted(l.title);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    {
        // Place the theme color toggle button neatly beside the close button in the top-right header.
        const float close_w = 44 * k;
        const float btn_fs = ImGui::GetFontSize() * 0.84f;
        const ImVec2 ts = ui::body()->CalcTextSizeA(btn_fs, FLT_MAX, 0, ui::theme().name);
        const float theme_w = 28 * k + 11 * k + 8 * k + ts.x;
        const float right_x = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - close_w - 12 * k - theme_w;
        ImGui::SameLine(right_x);
        if (ui::theme_button(k)) {
            s.theme = ui::theme_index();
            settings_changed(true);
        }
    }
    if (close_button(k)) c.open = false;
    ImGui::Dummy(ImVec2(0, 4 * k));

    const char* names[kTabs] = {l.video, l.audio, l.controls, l.overlay, l.system, "Network", "Speedrun",
                                kDebugLabels[std::clamp(s.language, 0, 12)].tab};
    int current = -1;
    // Tabs follow the keyboard / controller focus: moving onto a tab with the arrows or the D-pad shows it (next
    // frame), with no Enter or A needed. The mouse still selects by clicking. Only a change of focus moves the tab: L / R
    // select a tab without moving the focus, which must not pull it back.
    static int follow = -1, focused_before = -1, shown_tab = -1, hold_follow = 0;
    // L / R: the controller's cursor goes to the new page's first row, and following the focus pauses a few frames.
    // Otherwise the cursor, left on a row of the page that went away, fell back onto the first tab and the
    // tabs-follow-focus rule switched straight back (seen as "tab 1, tab 0" in the logs, before this change too).
    if (c.tab_request >= 0 && c.tab_request != shown_tab) rows::focus_first(), hold_follow = 4;
    static float page_a = 1, page_dir = 0, line_x0 = -1, line_x1 = -1;
    int follow_next = -1, focused = -1;
    ImVec2 tab_min[kTabs], tab_max[kTabs];
    const float footer = 64 * k;
    // Down with the cursor on the tab bar: the page's first row (Savage Wind found ImGui's own pick across the nested
    // panes unreliable, menu.mm tabs_body)
    if (focused_before >= 0 && ImGui::GetCurrentContext()->NavCursorVisible &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown) ||
         ImGui::IsKeyPressed(ImGuiKey_DownArrow)))
        rows::focus_first();
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the underline shows where the cursor is)
    if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_NoTooltip | ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int t = 0; t < kTabs; ++t) {
            ImGuiTabItemFlags tf = c.tab_request == t || follow == t ? ImGuiTabItemFlags_SetSelected : 0;
            char label[64];
            std::snprintf(label, sizeof label, "  %s  ###tab%d", names[t], t);
            const bool shown = ImGui::BeginTabItem(label, nullptr, tf);
            tab_min[t] = ImGui::GetItemRectMin(), tab_max[t] = ImGui::GetItemRectMax();
            if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) focused = t;
            if (!shown && focused == t && focused_before != t && hold_follow == 0) follow_next = t;
            if (shown && c.appearing) ImGui::SetItemDefaultFocus();  // navigation starts on the open tab
            if (!shown) continue;
            current = t;
            if (shown_tab != t) {  // another page: it slides in from the side it was asked from, and fades in
                page_dir = shown_tab < 0 ? 0.0f : t > shown_tab ? 1.0f : -1.0f;
                page_a = shown_tab < 0 ? 1.0f : 0.0f, shown_tab = t;
            }
            page_a = ui::approach(page_a, 1.0f, 14.0f);
            ImGui::PopStyleColor();  // (the rows want their own)
            ImGui::Dummy(ImVec2(0, 4 * k));
            const ImVec2 at = ImGui::GetCursorPos();
            const float page_w = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPos(ImVec2(at.x + (1.0f - ui::smoothstep(page_a)) * 36 * k * page_dir, at.y));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (0.15f + 0.85f * ui::smoothstep(page_a)));
            ImGui::BeginChild("page", {page_w, -footer}, ImGuiChildFlags_NavFlattened);
            switch (t) {
                case 0: video(c, l); break;
                case 1: audio(c, l); break;
                case 2: controls(c, l); break;
                case 3: overlay_tab(c); break;
                case 4: system_tab(c, l); break;
                case 5: network_tab(c); break;
                case 6: speedrun_tab(c); break;
                default: debug_tab(c); break;
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleColor();
    // the underline glides to the selected tab; brighter and thicker while the cursor is on the tab bar
    if (current >= 0) {
        const float x0 = tab_min[current].x + 6 * k, x1 = tab_max[current].x - 6 * k;
        if (line_x0 < 0 || c.appearing) line_x0 = x0, line_x1 = x1;
        line_x0 = ui::approach(line_x0, x0, 18.0f), line_x1 = ui::approach(line_x1, x1, 18.0f);
        const float on_bar = ui::eased(ImGui::GetID("##tabfocus"), focused >= 0 ? 1.0f : 0.0f, 18.0f);
        const float th = (3.0f + 2.0f * on_bar) * k, y = tab_max[current].y;
        wdl->AddRectFilled(ImVec2(line_x0, y - th), ImVec2(line_x1, y), ui::fade(ui::theme().accent, 0.75f + 0.25f * on_bar), th);
    }
    follow = follow_next;
    focused_before = focused;
    if (hold_follow > 0) hold_follow--;
    c.tab_request = current;  // out: the tab shown, for the shoulder buttons

    // the footer: what the buttons do, as Savage Wind's start screen shows them (badges)
    const ImVec2 wp = ImGui::GetWindowPos();
    const float fy = wp.y + ImGui::GetWindowHeight() - footer * 0.5f - 4 * k;
    wdl->AddLine(ImVec2(wp.x + 28 * k, fy - footer * 0.5f + 4 * k),
                 ImVec2(wp.x + ImGui::GetWindowWidth() - 28 * k, fy - footer * 0.5f + 4 * k), ui::fade(IM_COL32(255, 255, 255, 26)));
    const ui::Hint list[] = {{"A", "Select"}, {"B", "Back"}, {"L", ""}, {"R", "Tab"}, {"\xe2\x80\x93 +", "Close"},
                             {nullptr, "F1 / Esc"}};
    ui::hints(wdl, wp.x + ImGui::GetWindowWidth() * 0.5f, fy, 30 * k, list, int(sizeof list / sizeof list[0]));
    ImGui::End();
}

}  // namespace nx::overlay
