/* Included by the existing compositor: reuse Minish Cap's runtime-decoded
 * stone plate, red header chip, pale buttons and native message fonts. */
#include "updater.h"
#include "port_ua.h" /* tloz-tmc-ua: Update_FormatNotesUtf8 for Cyrillic changelogs */
#define UPDATE_LINE 129 /* tloz-tmc-ua: was 43; Cyrillic takes 2 bytes a letter */
#include <ctype.h>

static bool sUpdateConfirm, sUpdateNotes;
/* ua-release: the changelog scrolls by line; sUpdatePage is the first visible
 * line and sUpdatePages the largest first line (see Port_SecondScreen_3DS_UpdateKeys). */
#define UPDATE_VISIBLE_LINES PORT_3DS_CHANGELOG_PAGE_LINES
static unsigned sUpdatePage, sUpdatePages = 0;

static void UpdateUI_Reset(void) {
    UI_LOCK();
    sUpdateConfirm = sUpdateNotes = false;
    sUpdatePage = 0;
    UI_UNLOCK();
}

bool Port_SecondScreen_3DS_UpdateOpen(void) {
    UI_LOCK();
    bool open = sUi.tab == SS_TAB_SETTINGS && sUi.settingsPage == SS_SETTINGS_UPDATE;
    UI_UNLOCK();
    return open;
}

static void UpdateSmallText(const SSurf* s, const char* text, int x, int y, int style) {
    char label[128];
    snprintf(label,sizeof(label),"%s",text);
    for (char* c=label;*c;++c) *c=(char)toupper((unsigned char)*c);
    text=label;
    if (!Port_SecondScreenTheme_DrawBigText(s->px, s->w, s->h, s->stride, x, y, 1, style, text))
        DrawTextStr(s, text, x, y + 4, 1, Port_SecondScreenTheme_Color(SSC_MENU_INK), 0);
}

static int UpdateTextWidth(const char* text) {
    char label[128];
    snprintf(label,sizeof(label),"%s",text);
    for (char* c=label;*c;++c) *c=(char)toupper((unsigned char)*c);
    text=label;
    int w = Port_SecondScreenTheme_BigTextWidth(text, 1);
    return w ? w : TextWidthPx(text, 1);
}

/* tloz-tmc-ua: with the Ukrainian ROM the changelog body uses the message
 * font, which keeps both Cyrillic and Latin (the banner font holds Cyrillic
 * only, so Latin file names and hashes would be transliterated). */
static bool UpdateUaNotesFont(bool notes) { return notes && Port_IsUkrainianRom(); }
static int UpdateBodyWidth(const char* text, bool ua) {
    return ua ? Port_SecondScreenTheme_TextWidth(text, 1) : UpdateTextWidth(text);
}
static void UpdateBodyText(const SSurf* s, const char* text, int x, int y, int style, bool ua) {
    if (!ua || !Port_SecondScreenTheme_DrawText(s->px, s->w, s->h, s->stride, x, y, 1, style, text))
        UpdateSmallText(s, text, x, y, style);
}

static void UpdateCenteredText(const SSurf* s, const char* text, int cx, int y) {
    UpdateSmallText(s, text, cx - UpdateTextWidth(text)/2, y, SS_TEXT_NAVY);
}

static void UpdateButton(const SSurf* s, TargetList* tl, float x0, float y0,
                          float x1, float y1, const char* text, int action, bool pressed,
                          float u, int ts) {
    DrawMenuButton(s, x0, y0, x1, y1, "", pressed, 0, u, ts);
    UpdateSmallText(s, text, (int)((x0+x1-UpdateTextWidth(text))/2),
                    (int)((y0+y1)/2-8), SS_TEXT_NAVY);
    if (action) AddTarget(tl, x0, y0, x1, y1, action, 0);
}

static void PaintUpdatePanel(const SSurf* s, TargetList* tl, float x0, float y0,
                              float x1, float y1, float u, int ts) {
    UpdateStatus st; Updater_GetStatus(&st);
    bool confirm, notes;
    UI_LOCK(); confirm = sUpdateConfirm; notes = sUpdateNotes; UI_UNLOCK();
    float row = 26, gap = 4;
    int cx = (int)((x0+x1)/2);
    char text[96];
    snprintf(text, sizeof(text), "CHANNEL: %s", st.prerelease ? "PRE-RELEASE" : "STABLE");
    UpdateButton(s, tl, x0,y0,x1,y0+row,text,SS_ACT_UPDATE_CHANNEL,false,u,ts);
    UpdateButton(s, tl, x0,y0+row+gap,x1,y0+2*row+gap,
                 st.version[0] ? st.version : "NO RELEASE SELECTED",SS_ACT_UPDATE_RELEASE,notes,u,ts);
    const char* message = confirm ? "DOWNLOAD AND INSTALL" : st.message;
    if (!message[0]) message = "CHECK FOR UPDATE";
    // Long network errors are split at words inside the actual panel width.
    char first[80], second[80] = ""; snprintf(first,sizeof(first),"%s",message);
    if (UpdateTextWidth(first) > x1-x0) {
        char* split = strrchr(first,' ');
        while (split && UpdateTextWidth(first) > x1-x0) {
            *split = 0;
            snprintf(second,sizeof(second),"%s",message+strlen(first)+1);
            if (UpdateTextWidth(first) <= x1-x0) break;
            split = strrchr(first,' ');
        }
    }
    UpdateCenteredText(s,first,cx,(int)(y0+60));
    if (second[0]) UpdateCenteredText(s,second,cx,(int)(y0+75));
    else UpdateCenteredText(s,confirm ? "UNSAVED PROGRESS IS LOST" :
                              notes ? "CHANGELOG ON TOP SCREEN" : "TAP RELEASE FOR CHANGELOG",cx,(int)(y0+78));
    if (notes && !confirm && !Updater_Busy()) {
        UpdateButton(s,tl,x0,y0+99,x0+68,y0+120,"PREV",SS_ACT_UPDATE_PREV,false,u,ts);
        UpdateButton(s,tl,x1-68,y0+99,x1,y0+120,"NEXT",SS_ACT_UPDATE_NEXT,false,u,ts);
    }
    if (st.state == UPDATE_DOWNLOADING || st.state == UPDATE_VERIFYING || st.state == UPDATE_INSTALLING) {
        FillRect(s,(int)x0,(int)(y0+103),(int)x1,(int)(y0+111),Port_SecondScreenTheme_Color(SSC_MENU_STONE_DARK));
        FillRect(s,(int)x0,(int)(y0+103),(int)(x0+(x1-x0)*st.progress/100),(int)(y0+111),
                 Port_SecondScreenTheme_Color(SSC_RUPEE_GREEN));
    }
    const char* action = confirm ? "INSTALL UPDATE" : st.state == UPDATE_INSTALLING ? "PLEASE WAIT" :
        Updater_Busy() ? "CANCEL" : st.state == UPDATE_AVAILABLE ? "DOWNLOAD UPDATE" :
        st.state == UPDATE_DONE ? "UPDATE INSTALLED" : "CHECK FOR UPDATE";
    UpdateButton(s,tl,x0,y1-26,x1,y1,action,SS_ACT_UPDATE_ACTION,confirm,u,ts);
}

static bool sScreenHelp; /* ua-release: help shown for the Screen page */

static bool HandleUpdateTap(int action, int arg) {
    if (action == SS_ACT_SCREEN_HELP) { /* ua-release */
        UI_LOCK(); sScreenHelp = !sScreenHelp; UI_UNLOCK();
        return true;
    }
    if (!Port_SecondScreen_3DS_UpdateOpen()) return false;
    UpdateStatus st; Updater_GetStatus(&st);
    if (st.state == UPDATE_INSTALLING || st.state == UPDATE_DONE) return true;
    bool busy = Updater_Busy();
    if (action == SS_ACT_SETTINGS_BACK) {
        UI_LOCK(); bool confirm = sUpdateConfirm; sUpdateConfirm = false; UI_UNLOCK();
        if (confirm) return true;
        Updater_Cancel();
        if (busy) return true; // Wait for the worker before resuming the game.
        UpdateUI_Reset();
        return false;
    }
    if (action == SS_ACT_TAB) {
        if (busy) return true;
        UpdateUI_Reset(); return false;
    }
    if (action == SS_ACT_UPDATE_ACTION) {
        if (busy) { Updater_Cancel(); return true; }
        UI_LOCK(); bool confirm = sUpdateConfirm; sUpdateConfirm = false; UI_UNLOCK();
        if (confirm && st.state == UPDATE_AVAILABLE) Updater_Download();
        else if (st.state == UPDATE_AVAILABLE) {
            UI_LOCK(); sUpdateConfirm = true; UI_UNLOCK();
        } else Updater_Check();
        return true;
    }
    if (busy) return true;
    switch (action) {
        case SS_ACT_UPDATE_CHANNEL:
            UpdateUI_Reset(); Updater_SetChannel(!st.prerelease); break;
        case SS_ACT_UPDATE_RELEASE:
            if (st.version[0]) { UI_LOCK(); sUpdateNotes = true; sUpdatePage = 0; UI_UNLOCK(); }
            break;
        case SS_ACT_UPDATE_PREV:
            UI_LOCK(); sUpdatePage = sUpdatePage > UPDATE_VISIBLE_LINES ? sUpdatePage - UPDATE_VISIBLE_LINES : 0;
            UI_UNLOCK(); break;
        case SS_ACT_UPDATE_NEXT:
            UI_LOCK(); sUpdatePage = sUpdatePage + UPDATE_VISIBLE_LINES < sUpdatePages ?
                sUpdatePage + UPDATE_VISIBLE_LINES : sUpdatePages; UI_UNLOCK(); break;
        default: return true;
    }
    return true;
}


/* ua-release: the "?" on the Screen settings page shows on the top screen
 * what the current display choices do and what to expect from them. Players
 * took Pixel Perfect's small 1:1 frame, and Full View switching between
 * 400x240 rooms and that frame, for a broken display. */
extern bool Port_Config_3DSFullViewComboEnabled(void);

static bool ScreenHelpOpen(void) {
    UI_LOCK();
    const bool onPage = sUi.tab == SS_TAB_SETTINGS && sUi.settingsPage == SS_SETTINGS_SCREEN;
    if (!onPage) sScreenHelp = false; /* leaving the page closes it */
    const bool open = onPage && sScreenHelp;
    UI_UNLOCK();
    return open;
}

/* The "?" button opposite BACK in the Screen page header: shows the help on
 * the top screen in place of the game until it is tapped again. */
static void DrawScreenHelpButton(const SSurf* s, TargetList* tl, float x1, float y0, float h,
                                 float u, int32_t ts) {
    const bool open = ScreenHelpOpen();
    DrawMenuButton(s, x1 - h, y0, x1, y0 + h, "?", open, open, u, ts);
    AddTarget(tl, x1 - h, y0, x1, y0 + h, SS_ACT_SCREEN_HELP, 0);
}

bool Port_SecondScreen_3DS_TopPanelOpen(void) {
    return Port_SecondScreen_3DS_UpdateOpen() || ScreenHelpOpen();
}

/* Identifies the help text for the current settings, so it is rebuilt only
 * when one of them changes. */
static unsigned ScreenHelpSignature(void) {
    return 0x80000000u | ((unsigned)Port_Config_Get3DSAspectRatio() << 8) |
           ((unsigned)Port_Config_Get3DSDisplayStyle() << 4) |
           (Platform3DS_IsNew3DS() ? 2u : 0u) | (Port_Config_3DSFullViewComboEnabled() ? 1u : 0u);
}

static void ScreenHelpText(char* out, size_t cap) {
    const int aspect = Port_Config_Get3DSAspectRatio();
    const int style = Port_Config_Get3DSDisplayStyle();
    const bool fullView = Port_Config_3DSFullViewComboEnabled();
    const bool ua = Port_IsUkrainianRom();
    const char* now;
    if (ua) {
        static const char* const kAspect[] = { "ШИРОКИЙ", "ОРИГІНАЛ", "РОЗТЯГНУТО" };
        static const char* const kStyle[] = { "РОЗМИТТЯ", "БІЛІНІЙНИЙ", "ПІКСЕЛЬ 1:1" };
        char head[96];
        snprintf(head, sizeof(head), "Зараз: %s + %s.\n\n",
                 kAspect[aspect >= 0 && aspect < 3 ? aspect : 0], kStyle[style >= 0 && style < 3 ? style : 0]);
        if (fullView)
            now = "Експериментальний повний огляд (лише New 3DS). Кімнати надворі займають увесь "
                  "екран 400x240 без масштабування, підтримувані приміщення збільшено рівно вдвічі. "
                  "Діалоги, переходи, меню й заставки цей режим не підтримує: на цей час картинка "
                  "стає малим кадром 1:1 посередині, а потім повертається. Так задумано.\n\n"
                  "Щоб картинка завжди була на весь екран, оберіть ФІЛЬТР: РОЗМИТТЯ або БІЛІНІЙНИЙ.";
        else if (style == PORT_3DS_DISPLAY_PIXEL_PERFECT)
            now = "Кожен піксель гри показано одним пікселем екрана, без масштабування. Тому кадр "
                  "малий і стоїть посередині: екран 3DS має 240 рядків, а кадр GBA 160, і збільшити "
                  "його рівно вдвічі не вийде. Так задумано.\n\n"
                  "Щоб картинка була на весь екран, оберіть ФІЛЬТР: РОЗМИТТЯ або БІЛІНІЙНИЙ.";
        else if (style == PORT_3DS_DISPLAY_BILINEAR)
            now = aspect == PORT_3DS_ASPECT_STRETCH
                      ? "Кадр GBA розтягнуто на весь екран: спершу збільшено вдвічі піксель у піксель, "
                        "потім плавно зменшено. Чітко, але пропорції трохи ширші за оригінал."
                      : aspect == PORT_3DS_ASPECT_ORIGINAL
                            ? "Кадр на всю висоту екрана в пропорціях GBA, з вузькими чорними смугами з "
                              "боків. Спершу збільшено вдвічі піксель у піксель, потім плавно зменшено: чітко."
                            : "Гра показує ширшу ділянку світу й заповнює весь екран. Спершу збільшено "
                              "вдвічі піксель у піксель, потім плавно зменшено: чітко.";
        else
            now = aspect == PORT_3DS_ASPECT_STRETCH
                      ? "Кадр GBA розтягнуто на весь екран з м'яким згладжуванням."
                      : aspect == PORT_3DS_ASPECT_ORIGINAL
                            ? "Кадр на всю висоту екрана в пропорціях GBA, з вузькими чорними смугами з "
                              "боків, м'яко згладжений."
                            : "Гра показує ширшу ділянку світу й заповнює весь екран, м'яко згладжено.";
        snprintf(out, cap, "%s%s\n\n"
            "Що робить кожен пункт:\n\n"
            "СПІВВІДНОШЕННЯ - ширина картинки.\n"
            "- ШИРОКИЙ: ширша ділянка світу на весь екран.\n"
            "- ОРИГІНАЛ: пропорції GBA, смуги з боків.\n"
            "- РОЗТЯГНУТО: кадр GBA розтягнуто на весь екран.\n\n"
            "ФІЛЬТР - як збільшувати кадр.\n"
            "- РОЗМИТТЯ: на весь екран, м'яко.\n"
            "- БІЛІНІЙНИЙ: на весь екран, чіткіше.\n"
            "- ПІКСЕЛЬ 1:1: без збільшення, малий кадр посередині. "
            "Разом із ШИРОКИМ на New 3DS вмикає експериментальний повний огляд.\n\n"
            "ГЛИБИНА 3D - об'ємна картинка, коли піднято повзунок 3D: написи й панель лишаються "
            "на площині екрана, герої стоять трохи глибше, земля й далекі фони - ще глибше. "
            "СЛАБКА, СЕРЕДНЯ, СИЛЬНА - наскільки далеко розходяться шари; повзунок 3D плавно "
            "зменшує ефект. ВИМКНЕНО - картинка пласка за будь-якого положення повзунка. "
            "В експериментальному повному огляді 3D не працює.\n\n"
            "РЕЛЬЄФ 3D - стіни, дерева, меблі й усе інше, крізь що не можна пройти, "
            "підняті над підлогою, а не лежать у ній. Працює разом із ГЛИБИНОЮ 3D, "
            "окрім СПІВВІДНОШЕННЯ: ШИРОКИЙ.\n\n"
            "ПАНЕЛЬ ЗВЕРХУ - показувати чи ховати серця, рупії й кнопки предметів поверх гри; "
            "вони є й на нижньому екрані.\n\n"
            "КОРЕКЦІЯ КОЛЬОРУ - кольори як на екрані GBA, м'якші за яскраві оригінальні.\n\n"
            "ТЛО ПАНЕЛІ - візерунок фону нижнього екрана.", head, now);
        return;
    }
    static const char* const kAspect[] = { "WIDE", "ORIGINAL", "STRETCH" };
    static const char* const kStyle[] = { "BLUR", "BILINEAR", "PIXEL PERFECT" };
    if (fullView)
        now = "Experimental Full View (New 3DS only): outdoor rooms fill the 400x240 screen "
              "unscaled, supported interiors are doubled. Dialogues, transitions, menus and "
              "story scenes drop to the small 1:1 frame and come back. This is expected.\n\n"
              "For a full-screen picture all the time, pick Display Style Blur or Bilinear.";
    else if (style == PORT_3DS_DISPLAY_PIXEL_PERFECT)
        now = "One screen pixel per game pixel, no scaling, so the frame is small and centred: "
              "the screen has 240 lines and the GBA frame 160, too few for an exact 2x. "
              "This is expected.\n\nFor a full-screen picture, pick Display Style Blur or Bilinear.";
    else if (style == PORT_3DS_DISPLAY_BILINEAR)
        now = "The frame fills the screen height: doubled pixel for pixel, then smoothly scaled down. Sharp.";
    else
        now = "The frame fills the screen height with soft filtering.";
    snprintf(out, cap, "Now: %s + %s.\n\n%s\n\n"
        "Aspect Ratio: Wide shows more of the world, Original keeps GBA proportions, "
        "Stretch fills the screen with the GBA frame.\n\n"
        "Display Style: Blur is soft, Bilinear sharper, Pixel Perfect unscaled and small "
        "(with Wide on New 3DS: experimental Full View).\n\n"
        "3D Depth: with the 3D slider up, text and HUD stay on the screen plane, characters "
        "sit a little deeper, the ground and far backgrounds deeper still. Low, Medium and "
        "High set how far apart the layers go; Off keeps the picture flat.\n\n"
        "3D Relief: walls, trees, furniture and everything else that blocks the way "
        "stand above the floor instead of lying in it (not in the Wide aspect).",
        kAspect[aspect >= 0 && aspect < 3 ? aspect : 0], kStyle[style >= 0 && style < 3 ? style : 0], now);
}

/* Main-thread top painter. Theme caches have been initialized before either
 * screen worker starts; the top view reads only their immutable art. */
bool Port_SecondScreen_3DS_PaintUpdateTop(uint32_t* pixels, int stride) {
    const bool help = ScreenHelpOpen(); /* ua-release */
    if (!help && !Port_SecondScreen_3DS_UpdateOpen()) return false;
    UpdateStatus st; Updater_GetStatus(&st);
    bool notes; unsigned page;
    UI_LOCK(); notes = sUpdateNotes; page = sUpdatePage; UI_UNLOCK();
    /* ua-release: the help reads like release notes (font, scrolling), and
     * switching between it and the updater starts again at the top. */
    static bool lastHelp;
    if (help != lastHelp) { page = 0; UI_LOCK(); sUpdatePage = 0; UI_UNLOCK(); lastHelp = help; }
    if (help) notes = true;
    const unsigned rev = help ? ScreenHelpSignature() : st.revision;
    static unsigned paintedRevision = UINT32_MAX, paintedPage = UINT32_MAX;
    static bool paintedNotes;
    if (paintedRevision == rev && paintedPage == page && paintedNotes == notes) return false;
    paintedRevision=rev; paintedPage=page; paintedNotes=notes;
    static char lines[512][UPDATE_LINE]; /* tloz-tmc-ua */
    static unsigned count, revision = UINT32_MAX;
    static bool lastNotes;
    if (revision != rev || notes != lastNotes) {
        static char body[12289], formatted[512][UPDATE_LINE]; /* tloz-tmc-ua */
        if (help) ScreenHelpText(body,sizeof(body));
        else if (notes && st.version[0]) Updater_GetNotes(body,sizeof(body));
        else snprintf(body,sizeof(body),"Installed: v%s\n\n%s\n\nSelect a release below to read its changelog.\nChoose Stable or Pre-release to check that channel.",TMC_PORT_VERSION,st.message);
        const bool ua = Port_IsUkrainianRom(); /* tloz-tmc-ua */
        unsigned n = Update_FormatNotesUtf8(body,&formatted[0][0],UPDATE_LINE,ua ? UPDATE_LINE-1 : 42,512,ua);
        count = 0;
        for (unsigned i=0; i<n && count<512; i++) {
            const char* text = formatted[i];
            do {
                size_t take = strlen(text);
                char part[UPDATE_LINE]; snprintf(part,sizeof(part),"%s",text); /* tloz-tmc-ua */
                while (take && UpdateBodyWidth(part,UpdateUaNotesFont(notes))>354) part[--take]=0; /* tloz-tmc-ua */
                if (take<strlen(text)) {
                    char* space = strrchr(part,' ');
                    if (space && space!=part) { take = space-part; part[take]=0; }
                }
                while (take && ((unsigned char)text[take] & 0xC0) == 0x80) part[--take]=0; /* tloz-tmc-ua */
                if (!take && *text) { take=1; part[0]=*text; part[1]=0; }
                snprintf(lines[count++],sizeof(lines[0]),"%s",part); /* tloz-tmc-ua */
                text += take; while (*text==' ') ++text;
            } while (*text && count<512);
        }
        revision=rev; lastNotes=notes;
    }
    const unsigned maxTop = count > UPDATE_VISIBLE_LINES ? count - UPDATE_VISIBLE_LINES : 0;
    if (page>maxTop) page=maxTop;
    UI_LOCK(); sUpdatePages=maxTop; sUpdatePage=page; UI_UNLOCK();
    unsigned pages = (count+UPDATE_VISIBLE_LINES-1)/UPDATE_VISIBLE_LINES; if (!pages) pages=1;
    const unsigned shownPage = page >= maxTop ? pages : page/UPDATE_VISIBLE_LINES+1;
    SSurf s={pixels,400,240,stride};
    Port_SecondScreenTheme_DrawBackdrop(pixels,400,240,stride,0,0,400,240,2);
    Port_SecondScreenTheme_DrawPlate(pixels,400,240,stride,4,4,392,232,2);
    char title[80]; snprintf(title,sizeof(title),help ? "SCREEN" : !notes ? "UPDATE" : st.version[0] ? "CHANGELOG %s" : "CHANGELOG",st.version); /* ua-release: no trailing space to off-centre the chip */
    for(char* c=title;*c;++c) *c=(char)toupper((unsigned char)*c);
    DrawPanelHeaderChip(&s,200,14,title,1,1.0f/3);
    Port_SecondScreenTheme_DrawWell(pixels,400,240,stride,17,47,366,165,1);
    for (unsigned i=page;i<count && i<page+UPDATE_VISIBLE_LINES;i++)
        UpdateBodyText(&s,lines[i],23,49+(i-page)*18,SS_TEXT_INK,UpdateUaNotesFont(notes)); /* tloz-tmc-ua */
    if (maxTop) {
        /* Scroll bar at the well's right edge, clear of the 354 px text column. */
        const int trackY0=50, trackY1=208, trackH=trackY1-trackY0;
        int thumbH = trackH*UPDATE_VISIBLE_LINES/(int)count; if (thumbH<10) thumbH=10;
        const int thumbY = trackY0 + (trackH-thumbH)*(int)page/(int)maxTop;
        FillRect(&s,378,trackY0,381,trackY1,Port_SecondScreenTheme_Color(SSC_MENU_STONE));
        FillRect(&s,377,thumbY,382,thumbY+thumbH,Port_SecondScreenTheme_Color(SSC_BANNER_NAVY));
    }
    char footer[64]; snprintf(footer,sizeof(footer),"PAGE %u OF %u",shownPage,pages);
    const int footerW=UpdateTextWidth(footer)+24;
    const int footerY=211, footerH=24;
    DrawMenuButton(&s,200-footerW/2,footerY,200+(footerW+1)/2,footerY+footerH,"",0,0,1.f/3,1);
    // The Settings banner font has visible ink on rows 2..14. Center that
    // thirteen-pixel band, rather than the full sixteen-pixel glyph cell.
    UpdateSmallText(&s,footer,200-UpdateTextWidth(footer)/2,
                    footerY+(footerH-13)/2-2,SS_TEXT_NAVY);
    return true;
}

/* ua-release: D-pad scrolling of the open changelog (driven by platform_3ds.c
 * PollInput, which owns the key codes and the hold-to-repeat timing). */
bool Port_SecondScreen_3DS_ChangelogOpen(void) {
    if (ScreenHelpOpen()) return true;
    if (!Port_SecondScreen_3DS_UpdateOpen()) return false;
    UI_LOCK(); bool open = sUpdateNotes && !sUpdateConfirm; UI_UNLOCK();
    return open;
}
void Port_SecondScreen_3DS_ScrollChangelog(int lines) {
    UI_LOCK();
    if (lines < 0) sUpdatePage = sUpdatePage > (unsigned)-lines ? sUpdatePage - (unsigned)-lines : 0;
    else sUpdatePage = sUpdatePage + (unsigned)lines < sUpdatePages ? sUpdatePage + (unsigned)lines : sUpdatePages;
    UI_UNLOCK();
}
