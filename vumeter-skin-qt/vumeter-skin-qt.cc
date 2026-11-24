/*
 * Analog VU Meter (skinnable) - Qt port for Audacious 4.x
 *
 * Port of the old GTK2 "analog_vumeter" plugin. Keeps the original skin.cfg
 * format, the RMS/peak/decay maths and the config section name, but:
 *   - the GTK window / GdkPixmap code is replaced by one QWidget that
 *     Audacious docks (or floats) itself, so the skin title bar and the
 *     exit/config buttons are not used (the title bar image is cropped off),
 *   - the worker thread is gone: Audacious calls render_multi_pcm() from its
 *     own visualization timer, so the maths runs there and a QTimer handles
 *     the decay while playback is stopped,
 *   - the skin is switched from the right-click menu.
 *
 * Deliberately has no Q_OBJECT / signals / slots, so it needs no moc step.
 */

#include <algorithm>
#include <math.h>
#include <string.h>

#include <QAction>
#include <QContextMenuEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <libaudcore/drct.h>
#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>

#define CFG "analog_vumeter"

/* ------------------------------------------------------------------------ *
 *  Skin data
 * ------------------------------------------------------------------------ */

struct Zone
{
    float db;        // zone starts at this level
    QColor color;
    float h;         // lit height in px (0 = full bar height)
};

struct Module
{
    // 1 = analog needle, 2 = on/off image,
    // 3 = LED bar, 4 = counter, 5 = lamp, 6 = text label, 7 = rectangle
    int type = 0;
    bool enabled = false;
    int channel = 0;       // 0 = average, 1 = left, 2 = right
    int layer = 1;         // 1..5
    int width = 1;         // needle width
    int radius = 1;        // needle length
    int px = 0, py = 0;    // position (skin coordinates, rounded; used by needles/images)
    float fx = 0.f, fy = 0.f;   // exact position (decimals allowed; used by the newer elements)
    QColor color = Qt::white;
    float db_min = -300.f, db_max = 0.f;
    float angle_min = -180.f, angle_max = 180.f, angle_range = 0.f;
    QPixmap on_img, off_img;

    // ---- hardware-style elements (types 3..7)
    int source = 0;                // 0 = follow "Data source", 1 = RMS, 2 = peak
    float w = 0.f, h = 0.f;        // size (bar, rect, counter digit, chase arrow)
    QColor color2;                 // lamp "off" colour / rect gradient bottom
    QColor border;                 // rect border
    float corner = 0.f;            // rect corner radius
    QString text, family, lamp;
    float font_px = 10.f, spacing = 0.f;
    bool bold = false;
    int align = 0;                 // 0 left, 1 centre, 2 right (px = anchor, py = baseline)
    // bar
    int segments = 0;
    float seg_gap = 1.f;
    float offset_db = 0.f;         // calibration added to the (peak) level
    float extend_db = -999.f;      // loudness extension: RMS + extend_db can push the bar higher (-999 = off)
    float dim = 0.15f;             // brightness of unlit segments / digits
    float hold_ms = 0.f, hold_fall = 20.f;
    QVector<QPointF> scale;        // (dB, position in px along the bar)
    QVector<Zone> zones;
    QVector<float> seg_db;         // computed: dB threshold of every segment
    // counter / chase
    float pitch = 0.f, dot_w = 0.f, thick = 0.f;
    int count = 4;
    // runtime state (bar peak hold)
    float hold_db = -999.f;
    qint64 hold_t = 0;
};

struct Skin
{
    bool valid = false;
    QString name;
    int pathnum = 0;       // 0 = global data dir, 1 = user dir
    int width = 275, height = 116;
    QPixmap background;
    QPixmap titlebar;      // only used to know how much to crop off the top
    QColor window_color;   // optional fill around/behind the skin
    QPixmap overlay;       // optional glass/reflection layer drawn on top of everything
    float overlay_opacity = 1.f;
    QVector<Module> modules;
};

struct SkinRef
{
    int pathnum;
    QString name;
};

static QString skin_root(int pathnum)
{
    // Global:  <audacious data dir>/VU_Meter_skins
    // User:    <audacious user dir>/VU_Meter_skins   (%APPDATA%\audacious on Windows)
    const char * base = aud_get_path(pathnum == 0 ? AudPath::DataDir : AudPath::UserDir);
    return QString::fromUtf8(base) + "/VU_Meter_skins";
}

static QVector<SkinRef> scan_skins()
{
    QVector<SkinRef> list;
    for (int pathnum = 0; pathnum <= 1; pathnum++)
    {
        QDir root(skin_root(pathnum));
        for (const QString & name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        {
            QFileInfo cfg(root.filePath(name + "/skin.cfg"));
            if (cfg.isFile() && cfg.size() > 10)
                list.append({pathnum, name});
        }
    }
    return list;
}

static QColor parse_color(const QString & v, const QColor & fallback)
{
    QColor c(v.trimmed());
    return c.isValid() ? c : fallback;
}

static void parse_skin_key(Skin & s, const QString & key, const QString & val, const QDir & dir)
{
    if (key == "skin_size")
    {
        QStringList p = val.split(',');
        if (p.size() >= 2)
        {
            int w = p[0].trimmed().toInt(), h = p[1].trimmed().toInt();
            s.width = (w < 10 || w > 2000) ? 275 : w;
            s.height = (h < 10 || h > 2000) ? 116 : h;
        }
    }
    else if (key == "background_img")
        s.background = QPixmap(dir.filePath(val));
    else if (key == "titlebar_off_img")
        s.titlebar = QPixmap(dir.filePath(val));
    else if (key == "window_color")
        s.window_color = QColor(val.trimmed());
    else if (key == "overlay_img")
        s.overlay = QPixmap(dir.filePath(val));
    else if (key == "overlay_opacity")      // percent
        s.overlay_opacity = qBound(0.f, val.toFloat() / 100.f, 1.f);
    // titlebar_on_img, exit_button_pos, conf_button_pos: not used in the Qt port
}

static void parse_module_key(Module & m, const QString & key, const QString & val, const QDir & dir)
{
    if (key == "type")
    {
        QString v = val.toLower();
        if (v == "image") m.type = 2;
        else if (v == "analogvu") m.type = 1;
        else if (v == "bar") m.type = 3;
        else if (v == "counter") m.type = 4;
        else if (v == "lamp") m.type = 5;
        else if (v == "label") m.type = 6;
        else if (v == "rect") m.type = 7;
    }
    else if (key == "enabled")
        m.enabled = (val.toInt() == 1);
    else if (key == "channel")
    {
        int c = val.toInt();
        m.channel = (c >= 0 && c <= 2) ? c : 0;
    }
    else if (key == "layer")
    {
        int l = val.toInt();
        m.layer = (l >= 1 && l <= 5) ? l : 1;
    }
    else if (key == "position")
    {
        QStringList p = val.split(',');
        if (p.size() >= 2)
        {
            m.fx = p[0].trimmed().toFloat();
            m.fy = p[1].trimmed().toFloat();
            m.px = qRound(m.fx);
            m.py = qRound(m.fy);
        }
    }
    else if (key == "on_img")
        m.on_img = QPixmap(dir.filePath(val));
    else if (key == "off_img")
        m.off_img = QPixmap(dir.filePath(val));
    else if (key == "radius")
    {
        int r = val.toInt();
        m.radius = (r < 1 || r > 2000) ? 1 : r;
    }
    else if (key == "width")
    {
        int w = val.toInt();
        m.width = (w < 1 || w > 40) ? 1 : w;
    }
    else if (key == "color")
    {
        m.color = parse_color(val, Qt::white);
    }
    else if (key == "size")
    {
        QStringList sz = val.split(',');
        if (sz.size() >= 2)
        {
            m.w = sz[0].trimmed().toFloat();
            m.h = sz[1].trimmed().toFloat();
        }
    }
    else if (key == "source")
    {
        QString v = val.toLower();
        m.source = (v == "rms") ? 1 : (v == "peak") ? 2 : 0;
    }
    else if (key == "text")        m.text = val;
    else if (key == "font")        m.family = val;
    else if (key == "font_size")   m.font_px = qBound(3.f, val.toFloat(), 200.f);
    else if (key == "bold")        m.bold = (val.toInt() == 1);
    else if (key == "spacing")     m.spacing = val.toFloat();
    else if (key == "align")
    {
        QString v = val.toLower();
        m.align = (v == "center") ? 1 : (v == "right") ? 2 : 0;
    }
    else if (key == "color2")      m.color2 = parse_color(val, QColor());
    else if (key == "border")      m.border = parse_color(val, QColor());
    else if (key == "corner")      m.corner = qMax(0.f, val.toFloat());
    else if (key == "lamp")        m.lamp = val.toLower();
    else if (key == "segments")    m.segments = qBound(0, val.toInt(), 400);
    else if (key == "seg_gap")     m.seg_gap = qMax(0.f, val.toFloat());
    else if (key == "offset_db")   m.offset_db = val.toFloat();
    else if (key == "extend_db")   m.extend_db = val.toFloat();
    else if (key == "dim")         m.dim = qBound(0.f, val.toFloat(), 1.f);
    else if (key == "hold_ms")     m.hold_ms = qMax(0.f, val.toFloat());
    else if (key == "hold_fall")   m.hold_fall = qMax(1.f, val.toFloat());
    else if (key == "pitch")       m.pitch = qMax(0.f, val.toFloat());
    else if (key == "dot_w")       m.dot_w = qMax(0.f, val.toFloat());
    else if (key == "thick")       m.thick = qMax(0.f, val.toFloat());
    else if (key == "count")       m.count = qBound(1, val.toInt(), 20);
    else if (key == "scale")       // "dB:position, dB:position, ..."
    {
        m.scale.clear();
        for (const QString & pair : val.split(','))
        {
            QStringList kv = pair.split(':');
            if (kv.size() == 2)
                m.scale.append(QPointF(kv[0].trimmed().toDouble(), kv[1].trimmed().toDouble()));
        }
    }
    else if (key == "zones")       // "dB:#colour[:height], ..."
    {
        m.zones.clear();
        for (const QString & item : val.split(','))
        {
            QStringList kv = item.trimmed().split(':');
            if (kv.size() >= 2)
            {
                Zone z;
                z.db = kv[0].trimmed().toFloat();
                z.color = parse_color(kv[1], Qt::white);
                z.h = (kv.size() >= 3) ? kv[2].trimmed().toFloat() : 0.f;
                m.zones.append(z);
            }
        }
    }
    else if (key == "min_angle")
    {
        float a = val.toFloat();
        m.angle_min = (a < -180.f || a > 180.f) ? -180.f : a;
    }
    else if (key == "max_angle")
    {
        float a = val.toFloat();
        m.angle_max = (a < -180.f || a > 180.f) ? 180.f : a;
    }
    else if (key == "db_min")
    {
        float d = val.toFloat();
        m.db_min = (d < -300.f || d > 0.f) ? -300.f : d;
    }
    else if (key == "db_max")
    {
        float d = val.toFloat();
        m.db_max = (d < -300.f || d > 0.f) ? 0.f : d;
    }
}

// position (px along the bar) -> dB, interpolating between the skin's scale points
static float pos_to_db(const QVector<QPointF> & s, float pos)
{
    if (pos <= s.first().y())
        return (float)s.first().x();
    for (int i = 1; i < s.size(); i++)
        if (pos <= s[i].y())
        {
            const float d = (float)(s[i].y() - s[i - 1].y());
            if (d <= 0.f)
                return (float)s[i].x();
            const float t = (pos - (float)s[i - 1].y()) / d;
            return (float)(s[i - 1].x() + t * (s[i].x() - s[i - 1].x()));
        }
    return (float)s.last().x();
}

// work out the dB threshold of every LED segment
static void build_bar(Module & m)
{
    std::sort(m.scale.begin(), m.scale.end(), [](const QPointF & a, const QPointF & b) { return a.x() < b.x(); });
    std::sort(m.zones.begin(), m.zones.end(), [](const Zone & a, const Zone & b) { return a.db < b.db; });

    m.seg_db.clear();
    if (m.segments < 1 || m.w <= 0.f)
        return;

    const float pitch = m.w / m.segments;
    for (int i = 0; i < m.segments; i++)
    {
        const float pos = (i + 0.5f) * pitch;
        m.seg_db.append(m.scale.isEmpty() ? -40.f + (pos / m.w) * 58.f : pos_to_db(m.scale, pos));
    }
}

static bool load_skin(const SkinRef & ref, Skin & skin)
{
    QDir dir(skin_root(ref.pathnum) + "/" + ref.name);
    QFile file(dir.filePath("skin.cfg"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    skin = Skin();
    skin.name = ref.name;
    skin.pathnum = ref.pathnum;

    Module mod;
    bool inside = false;

    while (!file.atEnd())
    {
        QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line[0] == '#')
            continue;

        QString low = line.toLower();
        if (low == "<module>")
        {
            if (!inside) { mod = Module(); inside = true; continue; }
        }
        else if (low == "</module>")
        {
            if (inside)
            {
                if (mod.angle_min > mod.angle_max) mod.angle_min = mod.angle_max;
                if (mod.db_min > mod.db_max) mod.db_min = mod.db_max;

                // same precalculation as the original plugin
                mod.angle_min += 180.f;
                mod.angle_max += 180.f;
                mod.angle_range = mod.angle_max - mod.angle_min;

                if (mod.type == 3)
                    build_bar(mod);

                skin.modules.append(mod);
                inside = false;
                continue;
            }
        }

        int eq = line.indexOf('=');
        if (eq < 0)
            continue;

        QString key = line.left(eq).trimmed().toLower();
        QString val = line.mid(eq + 1).trimmed();

        if (inside) parse_module_key(mod, key, val, dir);
        else        parse_skin_key(skin, key, val, dir);
    }

    // glass layer: "overlay.png" next to skin.cfg is picked up automatically
    if (skin.overlay.isNull() && QFileInfo::exists(dir.filePath("overlay.png")))
        skin.overlay = QPixmap(dir.filePath("overlay.png"));

    skin.valid = true;
    return true;
}

/* ------------------------------------------------------------------------ *
 *  Audio analysis (same maths as the old worker thread)
 * ------------------------------------------------------------------------ */

static constexpr float FLOOR_DB = -93.f;

static int cfg_response() { int v = aud_get_int(CFG, "response_ms"); return (v < 10 || v > 500) ? 80 : v; }
static int cfg_fall()     { int v = aud_get_int(CFG, "peak_fall");   return (v < 5 || v > 100) ? 20 : v; }
static int cfg_source()   { int v = aud_get_int(CFG, "data_source"); return (v == 2) ? 2 : 1; }
static int cfg_smooth()   { int v = aud_get_int(CFG, "smoothing");   return (v < 0 || v > 300) ? 100 : v; }
// Gain is separate per data source: peaks read ~10 dB higher than RMS on music
static int cfg_gain(int src)
{
    int v = aud_get_int(CFG, src == 1 ? "rms_gain" : "peak_gain");
    return qBound(-20, v, 30);
}

static QElapsedTimer clock_ms;

// Latest raw snapshot written by render_multi_pcm()
static double snap_ms[2];             // mean square per channel
static float  snap_pk[2];             // sample peak per channel (dBFS)
static qint64 last_render_ms = 0, last_tick_ms = 0;

// Filtered state, published values: [0] = average/max, [1] = L, [2] = R
static double pow_ms[2];
static float  peak_hold[2];
static float  rms_values[3], peak_values[3];
static float  rms_fast[3], peak_fast[3];   // before display smoothing (used by LED bars)

static void reset_dsp()
{
    for (int c = 0; c < 2; c++)
    {
        snap_ms[c] = 0.0;  snap_pk[c] = FLOOR_DB;
        pow_ms[c] = 0.0;   peak_hold[c] = FLOOR_DB;
    }
    for (int i = 0; i < 3; i++)
        rms_values[i] = peak_values[i] = rms_fast[i] = peak_fast[i] = FLOOR_DB;
}

static float to_db(float x)
{
    if (x < 1e-5f)
        return FLOOR_DB;
    float db = 20.f * log10f(x);
    return (db > -0.1f) ? -0.1f : (db < FLOOR_DB ? FLOOR_DB : db);
}

// One ballistics step. dt is the real elapsed time in ms, so the response is
// independent of how often we are called.
//  RMS : first-order integration of power (time constant = "Response")
//  Peak: instant attack, constant fall in dB per second
static void advance(const double ms[2], const float pk[2], double dt)
{
    const double a = 1.0 - exp(-dt / (double)cfg_response());
    const float fall = (float)cfg_fall() * (float)(dt / 1000.0);
    float rms_db[2];

    for (int c = 0; c < 2; c++)
    {
        pow_ms[c] += a * (ms[c] - pow_ms[c]);
        if (pow_ms[c] < 1e-12) pow_ms[c] = 0.0;
        rms_db[c] = to_db((float)sqrt(pow_ms[c]));

        float f = peak_hold[c] - fall;
        peak_hold[c] = qMax(qMax(f, pk[c]), FLOOR_DB);
    }

    const float rms_t[3] = { to_db((float)sqrt((pow_ms[0] + pow_ms[1]) * 0.5)), rms_db[0], rms_db[1] };
    const float pk_t[3]  = { qMax(peak_hold[0], peak_hold[1]), peak_hold[0], peak_hold[1] };

    for (int i = 0; i < 3; i++)
    {
        rms_fast[i] = rms_t[i];
        peak_fast[i] = pk_t[i];
    }

    // Final smoothing of what the needle shows (in dB), so its motion stays even
    const double sm = (double)cfg_smooth();
    const float b = (sm < 1.0) ? 1.f : (float)(1.0 - exp(-dt / sm));
    for (int i = 0; i < 3; i++)
    {
        rms_values[i]  += b * (rms_t[i] - rms_values[i]);
        peak_values[i] += b * (pk_t[i]  - peak_values[i]);
    }
}

/* ------------------------------------------------------------------------ *
 *  Player state (counter, lamps) and drawing helpers for hardware-style skins
 * ------------------------------------------------------------------------ */

struct Status
{
    bool playing = false, paused = false, repeat = false, shuffle = false, mute = false;
    int sec = 0;
    int tape = 0;   // 0 = Normal, 1 = CrO2, 2 = Metal (derived from the file's quality)

    bool operator==(const Status & o) const
    {
        return playing == o.playing && paused == o.paused && repeat == o.repeat &&
               shuffle == o.shuffle && mute == o.mute && sec == o.sec && tape == o.tape;
    }
};

static Status status;

// Tape type from file quality: lossless = Metal, lossy >= 160 kbps = CrO2, else Normal
static int tape_type()
{
    int br = 0, sr = 0, ch = 0;
    aud_drct_get_info(br, sr, ch);

    String fn = aud_drct_get_filename();
    const char * c = (const char *) fn;
    QString f = c ? QString::fromUtf8(c) : QString();
    int q = f.indexOf('?');
    if (q >= 0)
        f.truncate(q);
    const int dot = f.lastIndexOf('.'), slash = f.lastIndexOf('/');
    const QString ext = (dot > slash && dot >= 0) ? f.mid(dot + 1).toUpper() : QString();

    static const QStringList lossless = {"FLAC", "WAV", "WAVE", "APE", "WV", "AIF",
                                         "AIFF", "TTA", "ALAC", "DSF", "DFF"};
    if (lossless.contains(ext))
        return 2;
    return ((br + 500) / 1000 >= 160) ? 1 : 0;
}

// Read the player state; returns true if anything changed
static bool poll_status(bool with_tape)
{
    Status s = status;
    s.playing = aud_drct_get_playing();
    s.paused = s.playing && aud_drct_get_paused();
    s.sec = s.playing ? aud_drct_get_time() / 1000 : 0;
    s.repeat = aud_get_bool(nullptr, "repeat");
    s.shuffle = aud_get_bool(nullptr, "shuffle");
    s.mute = (aud_drct_get_volume_main() == 0);
    if (with_tape)
        s.tape = (s.playing && aud_drct_get_ready()) ? tape_type() : 0;

    const bool changed = !(s == status);
    status = s;
    return changed;
}

static float module_level(const Module & m, bool fast)
{
    const int src = m.source ? m.source : cfg_source();
    const float * v = fast ? (src == 1 ? rms_fast : peak_fast) : (src == 1 ? rms_values : peak_values);
    const int ch = (m.channel >= 0 && m.channel <= 2) ? m.channel : 0;

    float lvl = v[ch] + m.offset_db;

    // Loudness extension: a digital peak can never exceed 0 dBFS, so the top of the
    // scale is reached through sustained loudness (RMS) instead of through peaks.
    if (m.extend_db > -900.f)
        lvl = qMax(lvl, (fast ? rms_fast : rms_values)[ch] + m.extend_db);

    return lvl;
}

static QColor with_alpha(const QColor & c, float a)
{
    QColor d = c;
    d.setAlphaF(a);
    return d;
}

static const Zone * zone_for(const Module & m, float db)
{
    const Zone * best = m.zones.isEmpty() ? nullptr : &m.zones[0];
    for (const Zone & z : m.zones)
        if (z.db <= db)
            best = &z;
    return best;
}

// Segmented LED bar with zone colours, per-zone lit height and a peak-hold segment
static void draw_bar(QPainter & p, const Module & m)
{
    if (m.segments < 1 || m.w <= 0.f || m.h <= 0.f || m.seg_db.size() != m.segments)
        return;

    const float lvl = module_level(m, true);
    const float pitch = m.w / m.segments;
    const float gap = (pitch - m.seg_gap >= 1.f) ? m.seg_gap : 0.f;
    const float sw = pitch - gap;
    const float bottom = m.fy + m.h;

    int hold_seg = -1;
    if (m.hold_ms > 0.f)
        for (int i = m.segments - 1; i >= 0; i--)
            if (m.seg_db[i] <= m.hold_db) { hold_seg = i; break; }

    p.setPen(Qt::NoPen);
    for (int i = 0; i < m.segments; i++)
    {
        const Zone * z = zone_for(m, m.seg_db[i]);
        const QColor c = z ? z->color : QColor(Qt::white);
        const float x = m.fx + i * pitch;

        p.setBrush(with_alpha(c, m.dim));                    // unlit segment
        p.drawRect(QRectF(x, m.fy, sw, m.h));

        if (lvl >= m.seg_db[i] || i == hold_seg)             // lit part
        {
            const float zh = (z && z->h > 0.f) ? qMin(z->h, m.h) : m.h;
            p.setBrush(c);
            p.drawRect(QRectF(x, bottom - zh, sw, zh));
        }
    }
}

// One seven-segment digit (digit < 0 draws all segments unlit)
static void draw_digit(QPainter & p, float x, float y, float w, float h, float t,
                       int digit, const QColor & on, const QColor & off)
{
    static const int mask[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    const int bits = (digit >= 0 && digit <= 9) ? mask[digit] : 0;

    const float g = t * 0.12f;
    const float cyT = y + t * 0.5f, cyM = y + h * 0.5f, cyB = y + h - t * 0.5f;
    const float cxL = x + t * 0.5f, cxR = x + w - t * 0.5f;

    auto hseg = [&](float cy) {
        QPolygonF q;
        const float x0 = x + t * 0.5f + g, x1 = x + w - t * 0.5f - g;
        q << QPointF(x0 - t * 0.5f, cy) << QPointF(x0, cy - t * 0.5f) << QPointF(x1, cy - t * 0.5f)
          << QPointF(x1 + t * 0.5f, cy) << QPointF(x1, cy + t * 0.5f) << QPointF(x0, cy + t * 0.5f);
        return q;
    };
    auto vseg = [&](float cx, float y0, float y1) {
        QPolygonF q;
        q << QPointF(cx, y0) << QPointF(cx + t * 0.5f, y0 + t * 0.5f) << QPointF(cx + t * 0.5f, y1 - t * 0.5f)
          << QPointF(cx, y1) << QPointF(cx - t * 0.5f, y1 - t * 0.5f) << QPointF(cx - t * 0.5f, y0 + t * 0.5f);
        return q;
    };

    const QPolygonF segs[7] = {
        hseg(cyT),                         // a  top
        vseg(cxR, cyT + g, cyM - g),       // b  upper right
        vseg(cxR, cyM + g, cyB - g),       // c  lower right
        hseg(cyB),                         // d  bottom
        vseg(cxL, cyM + g, cyB - g),       // e  lower left
        vseg(cxL, cyT + g, cyM - g),       // f  upper left
        hseg(cyM)                          // g  middle
    };

    p.setPen(Qt::NoPen);
    for (int i = 0; i < 7; i++)
    {
        p.setBrush(((bits >> i) & 1) ? on : off);
        p.drawPolygon(segs[i]);
    }
}

// Tape counter: elapsed time as MM.SS
static void draw_counter(QPainter & p, const Module & m)
{
    const int mm = (status.sec / 60) % 100, ss = status.sec % 60;
    const int d[4] = {mm / 10, mm % 10, ss / 10, ss % 10};

    const float dw = m.w > 0.f ? m.w : 14.f, dh = m.h > 0.f ? m.h : 20.f;
    const float pitch = m.pitch > 0.f ? m.pitch : dw * 1.15f;
    const float dotw = m.dot_w > 0.f ? m.dot_w : dw * 0.8f;
    const float t = m.thick > 0.f ? m.thick : qMax(1.5f, dh * 0.15f);
    const QColor off = with_alpha(m.color, m.dim);

    float x = m.fx;
    for (int i = 0; i < 4; i++)
    {
        draw_digit(p, x, m.fy, dw, dh, t, d[i], m.color, off);
        if (i == 1)   // the dot between minutes and seconds
        {
            const float dot = qMax(2.f, t);
            p.setPen(Qt::NoPen);
            p.setBrush(m.color);
            p.drawRect(QRectF(x + dw + (pitch - dw + dotw) / 2.f - dot / 2.f, m.fy + dh - dot, dot, dot));
            x += pitch + dotw;
        }
        else
            x += pitch;
    }
}

// Text anchored at (px, baseline py); align: 0 left, 1 centre, 2 right
static void draw_text(QPainter & p, const Module & m, const QColor & c)
{
    if (m.text.isEmpty())
        return;

    QFont f = p.font();
    if (!m.family.isEmpty())
        f.setFamily(m.family);
    f.setPixelSize(qMax(4, qRound(m.font_px)));
    f.setBold(m.bold);
    f.setHintingPreference(QFont::PreferNoHinting);
    if (m.spacing != 0.f)
        f.setLetterSpacing(QFont::AbsoluteSpacing, m.spacing);
    p.setFont(f);
    p.setPen(c);

    const QFontMetricsF fm(f);
    const qreal tw = fm.horizontalAdvance(m.text);
    qreal x = m.fx;
    if (m.align == 1) x -= tw / 2.0;
    else if (m.align == 2) x -= tw;
    p.drawText(QPointF(x, m.fy), m.text);
}

static bool lamp_lit(const Module & m)
{
    const QString & k = m.lamp;
    if (k == "repeat")  return status.repeat;
    if (k == "shuffle") return status.shuffle;
    if (k == "mute")    return status.mute;
    if (k == "playing") return status.playing;
    if (k == "paused")  return status.paused;
    if (k == "normal")  return status.tape == 0;
    if (k == "cro2")    return status.tape == 1;
    if (k == "metal")   return status.tape == 2;
    if (k == "always")  return true;
    return false;       // "off" or unknown: decorative, never lit
}

// Lamp = text that lights up, or the animated "chase" arrows
static void draw_lamp(QPainter & p, const Module & m, int chase_step)
{
    const QColor off = m.color2.isValid() ? m.color2 : with_alpha(m.color, 0.25f);

    if (m.lamp == "chase")
    {
        const float w = m.w > 0.f ? m.w : 7.f, h = m.h > 0.f ? m.h : 10.f;
        const float pitch = m.pitch > 0.f ? m.pitch : w + 2.f;
        const bool run = status.playing && !status.paused;
        const int lit = run ? (m.count - 1 - (chase_step % m.count)) : -1;

        p.setPen(Qt::NoPen);
        for (int i = 0; i < m.count; i++)
        {
            const float x = m.fx + i * pitch;
            QPolygonF tri;
            tri << QPointF(x, m.fy + h / 2) << QPointF(x + w, m.fy) << QPointF(x + w, m.fy + h);
            p.setBrush(i == lit ? m.color : off);
            p.drawPolygon(tri);
        }
        return;
    }

    draw_text(p, m, lamp_lit(m) ? m.color : off);
}

// Rectangle: flat colour, or vertical gradient colour -> color2, optional border
static void draw_rect(QPainter & p, const Module & m)
{
    QRectF r(m.fx, m.fy, m.w, m.h);
    if (m.color2.isValid())
    {
        QLinearGradient g(r.topLeft(), r.bottomLeft());
        g.setColorAt(0.0, m.color);
        g.setColorAt(1.0, m.color2);
        p.setBrush(g);
    }
    else
        p.setBrush(m.color);

    if (m.border.isValid())
    {
        p.setPen(QPen(m.border, 1));
        r.adjust(0.5, 0.5, -0.5, -0.5);
    }
    else
        p.setPen(Qt::NoPen);

    p.drawRoundedRect(r, m.corner, m.corner);
}

/* ------------------------------------------------------------------------ *
 *  The widget
 * ------------------------------------------------------------------------ */

class VUWidget : public QWidget
{
public:
    VUWidget(QWidget * parent = nullptr) : QWidget(parent)
    {
        // restore last skin, otherwise take the first one found
        SkinRef ref{-1, QString()};
        String saved = aud_get_str(CFG, "skin");   // format "pathnum:name"
        const char * saved_c = (const char *) saved;
        QString s = saved_c ? QString::fromUtf8(saved_c) : QString();
        int colon = s.indexOf(':');
        if (colon > 0)
            ref = {s.left(colon).toInt(), s.mid(colon + 1)};

        if (ref.pathnum < 0 || !load_skin(ref, m_skin))
        {
            QVector<SkinRef> all = scan_skins();
            for (const SkinRef & r : all)
                if (load_skin(r, m_skin))
                    break;
        }
        m_has_chase = has_chase();
        poll_status(true);
        resize(sizeHint());

        QObject::connect(&m_timer, &QTimer::timeout, [this]() { tick(); });
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.start(16);   // ~60 Hz redraw
    }

    // Preferred size is the skin's own size (never wider than 640 px); the skin is
    // scaled to fit whatever size the widget actually gets
    QSize sizeHint() const override
    {
        if (!m_skin.valid) return QSize(275, 116);
        const int w = m_skin.width, h = m_skin.height - titlebar_height();
        const double k = (w > 640) ? 640.0 / w : 1.0;
        return QSize(qRound(w * k), qRound(h * k));
    }

    QSize minimumSizeHint() const override { return QSize(120, 60); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), m_skin.window_color.isValid() ? QBrush(m_skin.window_color) : palette().window());

        if (!m_skin.valid)
        {
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                       QString("No VU meter skins found.\nPut skin folders in:\n%1\nor\n%2")
                           .arg(skin_root(0), skin_root(1)));
            return;
        }

        const int tb = titlebar_height();
        const double w = m_skin.width, h = m_skin.height - tb;

        // scale the skin to fit the widget (keeping its aspect ratio), centre it,
        // and crop the (unused) title bar
        const double k = qMin(width() / w, height() / h);
        p.translate((width() - w * k) / 2.0, (height() - h * k) / 2.0);
        p.scale(k, k);
        p.setClipRect(QRectF(0, 0, w, h));
        p.translate(0, -tb);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);

        if (!m_skin.background.isNull())
            p.drawPixmap(0, tb, m_skin.background);

        const float * values = (cfg_source() == 1) ? rms_values : peak_values;
        const float rad = (float)M_PI / 180.f;

        for (int layer = 1; layer <= 5; layer++)
            for (const Module & m : m_skin.modules)
            {
                if (!m.enabled || m.layer != layer) continue;

                if (m.type == 1 || m.type == 2)
                {
                    if (m.channel < 0 || m.channel > 2) continue;
                    float v = values[m.channel];

                    if (m.type == 1)   // analog needle
                    {
                        const float vn = v + (float)cfg_gain(cfg_source());   // calibration gain: needles only
                        float angle;
                        if (vn <= m.db_min)      angle = m.angle_min;
                        else if (vn >= m.db_max) angle = m.angle_max;
                        else
                        {
                            angle = log10f(fabsf(vn - 1.f)) / log10f(fabsf(m.db_min) + 1.f);
                            angle = m.angle_range * (1.f - angle) + m.angle_min;
                        }

                        int x = m.px + (int)floorf((float)m.radius * cosf((90.f + angle) * rad));
                        int y = m.py + (int)floorf((float)m.radius * sinf((90.f + angle) * rad));

                        p.setPen(QPen(m.color, m.width, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
                        p.drawLine(m.px, m.py, x, y);
                    }
                    else               // on/off image
                    {
                        const QPixmap & img = (v >= m.db_min && v <= m.db_max) ? m.on_img : m.off_img;
                        if (!img.isNull())
                            p.drawPixmap(m.px, m.py, img);
                    }
                }
                else if (m.type == 3) draw_bar(p, m);
                else if (m.type == 4) draw_counter(p, m);
                else if (m.type == 5) draw_lamp(p, m, m_chase);
                else if (m.type == 6) draw_text(p, m, m.color);
                else if (m.type == 7) draw_rect(p, m);
            }

        // glass overlay: stretched over the whole skin, on top of everything.
        // The scaled copy is cached, so it is only re-scaled when the size changes.
        if (!m_skin.overlay.isNull())
        {
            const qreal dpr = devicePixelRatioF();
            const QSize target(qMax(1, qRound(w * k * dpr)), qMax(1, qRound(h * k * dpr)));
            if (m_overlay_cache.size() != target || m_overlay_key != m_skin.overlay.cacheKey())
            {
                m_overlay_cache = m_skin.overlay.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                m_overlay_cache.setDevicePixelRatio(dpr);
                m_overlay_key = m_skin.overlay.cacheKey();
            }

            p.resetTransform();
            p.setClipping(false);
            p.setOpacity(m_skin.overlay_opacity);
            p.drawPixmap(QPointF((width() - w * k) / 2.0, (height() - h * k) / 2.0), m_overlay_cache);
        }
    }

    void contextMenuEvent(QContextMenuEvent * e) override
    {
        QMenu menu(this);
        for (const SkinRef & r : scan_skins())
        {
            QAction * a = menu.addAction(r.name + (r.pathnum == 1 ? " (user)" : ""));
            a->setCheckable(true);
            a->setChecked(m_skin.valid && r.pathnum == m_skin.pathnum && r.name == m_skin.name);
            SkinRef copy = r;
            QObject::connect(a, &QAction::triggered, [this, copy]() { set_skin(copy); });
        }
        menu.exec(e->globalPos());
    }

private:
    int titlebar_height() const
    {
        return m_skin.titlebar.isNull() ? 0 : m_skin.titlebar.height();
    }

    void set_skin(const SkinRef & ref)
    {
        Skin s;
        if (!load_skin(ref, s))
            return;
        m_skin = s;
        aud_set_str(CFG, "skin", QString("%1:%2").arg(ref.pathnum).arg(ref.name).toUtf8().constData());
        m_has_chase = has_chase();
        updateGeometry();
        update();
    }

    bool has_chase() const
    {
        for (const Module & m : m_skin.modules)
            if (m.type == 5 && m.lamp == "chase")
                return true;
        return false;
    }

    // Bar peak-hold markers: jump up with the level, stay for hold_ms, then fall
    bool update_holds(qint64 now, double dt)
    {
        bool active = false;
        for (Module & m : m_skin.modules)
        {
            if (m.type != 3 || m.hold_ms <= 0.f || m.seg_db.isEmpty())
                continue;

            const float v = module_level(m, true);
            if (v >= m.hold_db)
            {
                m.hold_db = v;
                m.hold_t = now;
            }
            else if (now - m.hold_t > (qint64)m.hold_ms)
            {
                m.hold_db -= m.hold_fall * (float)(dt / 1000.0);
                if (m.hold_db < v)
                    m.hold_db = v;
            }

            if (m.hold_db > m.seg_db[0] - 1.f)
                active = true;
        }
        return active;
    }

    // Runs ~60 times a second: feeds the latest snapshot (or silence, if no
    // audio has arrived for 100 ms) through the ballistics, refreshes the
    // player state a few times a second, and redraws when something moved.
    void tick()
    {
        const qint64 now = clock_ms.elapsed();
        const double dt = qBound(1.0, (double)(now - last_tick_ms), 100.0);
        last_tick_ms = now;

        const bool live = (now - last_render_ms) <= 100;
        const double ms[2] = {live ? snap_ms[0] : 0.0, live ? snap_ms[1] : 0.0};
        const float pk[2] = {live ? snap_pk[0] : FLOOR_DB, live ? snap_pk[1] : FLOOR_DB};
        advance(ms, pk, dt);

        bool changed = false;
        if (now - m_last_poll >= 100)
        {
            m_last_poll = now;
            const bool with_tape = (now - m_last_tape >= 500);
            if (with_tape)
                m_last_tape = now;
            changed = poll_status(with_tape);
        }

        if (m_has_chase)
        {
            const int step = (status.playing && !status.paused) ? (int)((now / 140) % 1000) : 0;
            if (step != m_chase)
            {
                m_chase = step;
                changed = true;
            }
        }

        const bool holding = update_holds(now, dt);

        if (live || changed || holding || rms_values[0] > FLOOR_DB + 0.05f || peak_values[0] > FLOOR_DB + 0.05f)
            update();
    }

    Skin m_skin;
    QTimer m_timer;
    qint64 m_last_poll = 0, m_last_tape = 0;
    int m_chase = 0;
    bool m_has_chase = false;
    QPixmap m_overlay_cache;
    qint64 m_overlay_key = 0;
};

static VUWidget * widget = nullptr;

/* ------------------------------------------------------------------------ *
 *  Plugin glue
 * ------------------------------------------------------------------------ */

class VUMeterPlugin : public VisPlugin
{
public:
    static const char about[];
    static const PreferencesWidget widgets[];
    static const ComboItem source_items[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {N_("Analog VU Meter"), PACKAGE, about, &prefs, PluginQtOnly};

    constexpr VUMeterPlugin() : VisPlugin(info, Visualizer::MultiPCM) {}

    bool init();
    void clear();
    void render_multi_pcm(const float * pcm, int channels);
    void * get_qt_widget();
};

EXPORT VUMeterPlugin aud_plugin_instance;

const char VUMeterPlugin::about[] =
    N_("Analog VU Meter (skinnable)\n\n"
       "Qt port of the original GTK plugin.\n"
       "Right-click the meter to choose a skin.\n"
       "Skins go in the VU_Meter_skins folder of the Audacious user directory.");

const ComboItem VUMeterPlugin::source_items[] = {
    ComboItem(N_("RMS"), 1),
    ComboItem(N_("Peak"), 2)
};

const PreferencesWidget VUMeterPlugin::widgets[] = {
    WidgetSpin(N_("RMS response:"), WidgetInt(CFG, "response_ms"), {10, 500, 5, N_("ms")}),
    WidgetSpin(N_("Peak fall:"), WidgetInt(CFG, "peak_fall"), {5, 100, 1, N_("dB/s")}),
    WidgetSpin(N_("Needle smoothing:"), WidgetInt(CFG, "smoothing"), {0, 300, 5, N_("ms")}),
    WidgetSpin(N_("RMS gain:"), WidgetInt(CFG, "rms_gain"), {-20, 30, 1, N_("dB")}),
    WidgetSpin(N_("Peak gain:"), WidgetInt(CFG, "peak_gain"), {-20, 30, 1, N_("dB")}),
    WidgetCombo(N_("Data source:"), WidgetInt(CFG, "data_source"), {{source_items}})
};

const PluginPreferences VUMeterPlugin::prefs = {{widgets}};

static const char * const defaults[] = {
    "response_ms", "120",
    "smoothing", "100",
    "peak_fall", "20",
    "data_source", "1",
    "rms_gain", "6",
    "peak_gain", "0",
    "skin", "",
    nullptr
};

bool VUMeterPlugin::init()
{
    aud_config_set_defaults(CFG, defaults);
    clock_ms.start();
    reset_dsp();
    return true;
}

void VUMeterPlugin::clear()
{
    // Playback stopped: the widget timer lets the needles fall back
    last_render_ms = 0;
}

void VUMeterPlugin::render_multi_pcm(const float * pcm, int channels)
{
    if (!widget || channels < 1)
        return;

    const int N = 512;   // Audacious delivers 512 frames per call

    for (int ch = 0; ch < 2; ch++)
    {
        const int c = (ch < channels) ? ch : 0;   // mono -> duplicate to both
        double sum = 0.0;
        float pk = 0.f;

        for (int i = 0; i < N; i++)
        {
            const float v = pcm[i * channels + c];
            sum += (double)v * v;
            const float av = fabsf(v);
            if (av > pk) pk = av;
        }

        snap_ms[ch] = sum / N;
        snap_pk[ch] = to_db(pk);
    }

    last_render_ms = clock_ms.elapsed();
}

void * VUMeterPlugin::get_qt_widget()
{
    if (widget)
        return widget;

    widget = new VUWidget;
    QObject::connect(widget, &QObject::destroyed, []() { widget = nullptr; });
    return widget;
}
