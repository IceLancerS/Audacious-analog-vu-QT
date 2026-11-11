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

#include <math.h>
#include <string.h>

#include <QAction>
#include <QContextMenuEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>

#define CFG "analog_vumeter"

/* ------------------------------------------------------------------------ *
 *  Skin data
 * ------------------------------------------------------------------------ */

struct Module
{
    int type = 0;          // 1 = analog needle, 2 = on/off image
    bool enabled = false;
    int channel = 0;       // 0 = average, 1 = left, 2 = right
    int layer = 1;         // 1..5
    int width = 1;         // needle width
    int radius = 1;        // needle length
    int px = 0, py = 0;    // position (skin coordinates)
    QColor color = Qt::white;
    float db_min = -300.f, db_max = 0.f;
    float angle_min = -180.f, angle_max = 180.f, angle_range = 0.f;
    QPixmap on_img, off_img;
};

struct Skin
{
    bool valid = false;
    QString name;
    int pathnum = 0;       // 0 = global data dir, 1 = user dir
    int width = 275, height = 116;
    QPixmap background;
    QPixmap titlebar;      // only used to know how much to crop off the top
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

static void parse_skin_key(Skin & s, const QString & key, const QString & val, const QDir & dir)
{
    if (key == "skin_size")
    {
        QStringList p = val.split(',');
        if (p.size() >= 2)
        {
            int w = p[0].trimmed().toInt(), h = p[1].trimmed().toInt();
            s.width = (w < 10 || w > 800) ? 275 : w;
            s.height = (h < 10 || h > 800) ? 116 : h;
        }
    }
    else if (key == "background_img")
        s.background = QPixmap(dir.filePath(val));
    else if (key == "titlebar_off_img")
        s.titlebar = QPixmap(dir.filePath(val));
    // titlebar_on_img, exit_button_pos, conf_button_pos: not used in the Qt port
}

static void parse_module_key(Module & m, const QString & key, const QString & val, const QDir & dir)
{
    if (key == "type")
    {
        QString v = val.toLower();
        if (v == "image") m.type = 2;
        else if (v == "analogvu") m.type = 1;
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
            m.px = p[0].trimmed().toInt();
            m.py = p[1].trimmed().toInt();
        }
    }
    else if (key == "on_img")
        m.on_img = QPixmap(dir.filePath(val));
    else if (key == "off_img")
        m.off_img = QPixmap(dir.filePath(val));
    else if (key == "radius")
    {
        int r = val.toInt();
        m.radius = (r < 1 || r > 100) ? 1 : r;
    }
    else if (key == "width")
    {
        int w = val.toInt();
        m.width = (w < 1 || w > 10) ? 1 : w;
    }
    else if (key == "color")
    {
        if (val.size() == 7 && val[0] == '#') m.color = QColor(val);
        else m.color = Qt::white;
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

static void reset_dsp()
{
    for (int c = 0; c < 2; c++)
    {
        snap_ms[c] = 0.0;  snap_pk[c] = FLOOR_DB;
        pow_ms[c] = 0.0;   peak_hold[c] = FLOOR_DB;
    }
    for (int i = 0; i < 3; i++)
        rms_values[i] = peak_values[i] = FLOOR_DB;
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
        resize(sizeHint());

        QObject::connect(&m_timer, &QTimer::timeout, [this]() { tick(); });
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.start(16);   // ~60 Hz redraw
    }

    QSize sizeHint() const override
    {
        if (!m_skin.valid) return QSize(275, 116);
        return QSize(m_skin.width, m_skin.height - titlebar_height());
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().window());

        if (!m_skin.valid)
        {
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                       QString("No VU meter skins found.\nPut skin folders in:\n%1\nor\n%2")
                           .arg(skin_root(0), skin_root(1)));
            return;
        }

        const int tb = titlebar_height();
        const int w = m_skin.width, h = m_skin.height - tb;

        // centre the skin in the widget, crop the (unused) title bar
        p.translate((width() - w) / 2, (height() - h) / 2);
        p.setClipRect(0, 0, w, h);
        p.translate(0, -tb);
        p.setRenderHint(QPainter::Antialiasing);

        if (!m_skin.background.isNull())
            p.drawPixmap(0, tb, m_skin.background);

        const float * values = (cfg_source() == 1) ? rms_values : peak_values;
        const float rad = (float)M_PI / 180.f;

        for (int layer = 1; layer <= 5; layer++)
            for (const Module & m : m_skin.modules)
            {
                if (!m.enabled || m.layer != layer) continue;
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
                else if (m.type == 2)   // on/off image
                {
                    const QPixmap & img = (v >= m.db_min && v <= m.db_max) ? m.on_img : m.off_img;
                    if (!img.isNull())
                        p.drawPixmap(m.px, m.py, img);
                }
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
        setMinimumSize(sizeHint());
        updateGeometry();
        update();
    }

    // Runs ~60 times a second: feeds the latest snapshot (or silence, if no
    // audio has arrived for 100 ms) through the ballistics and redraws.
    void tick()
    {
        const qint64 now = clock_ms.elapsed();
        const double dt = qBound(1.0, (double)(now - last_tick_ms), 100.0);
        last_tick_ms = now;

        const bool live = (now - last_render_ms) <= 100;
        const double ms[2] = {live ? snap_ms[0] : 0.0, live ? snap_ms[1] : 0.0};
        const float pk[2] = {live ? snap_pk[0] : FLOOR_DB, live ? snap_pk[1] : FLOOR_DB};
        advance(ms, pk, dt);

        if (live || rms_values[0] > FLOOR_DB + 0.05f || peak_values[0] > FLOOR_DB + 0.05f)
            update();
    }

    Skin m_skin;
    QTimer m_timer;
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
