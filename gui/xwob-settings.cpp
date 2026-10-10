// xwob-settings - minimal GUI for ~/.config/xwob/xwob.ini
//
// Edits the one config file that both xwob backends (X11 and Wayland) read.
//  * Preview shows the bar with the settings in the window, saved or not, by
//    running its own throw-away xwob instance on a temporary copy.
//  * Apply writes xwob.ini and runs `xwob --reload`, so running bars pick the
//    new settings up immediately, without logging out.
//
// Qt6 Widgets only. No Q_OBJECT classes, so no moc step is needed:
//   g++ -std=c++17 -fPIC $(pkg-config --cflags --libs Qt6Widgets) xwob-settings.cpp
//
// Released under the same license as xwob (see LICENSE.md).

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTimer>
#include <QProcess>
#include <QVBoxLayout>

#include <functional>
#include <map>
#include <vector>

namespace {

// ---- defaults: identical to the built-in defaults of both backends --------

const std::map<QString, QString> kDefaults = {
    {"max", "100"},
    {"timeout", "1000"},
    {"show_percentage", "yes"},
    {"width", "400"},
    {"height", "50"},
    {"border_offset", "4"},
    {"border_size", "4"},
    {"bar_padding", "4"},
    {"orientation", "horizontal"},
    {"anchor", "center"},
    {"margin", "0"},
    {"overflow_mode", "wrap"},
    {"bar_color", "ffffff"},
    {"border_color", "ffffff"},
    {"background_color", "00000090"},
    {"overflow_bar_color", "ff0000"},
    {"overflow_border_color", "ff0000"},
    {"overflow_background_color", "00000090"},
};
const std::map<QString, QString> kAltDefaults = {
    {"bar_color", "555555"},
    {"border_color", "555555"},
    {"overflow_bar_color", "550000"},
    {"overflow_border_color", "550000"},
};

// Keys the window edits. Anything else in the file is kept as it is.
const QStringList kGlobalOrder = {
    "timeout", "max", "show_percentage", "width", "height", "border_offset",
    "border_size", "bar_padding", "orientation", "anchor", "margin",
    "overflow_mode", "bar_color", "border_color", "background_color",
    "overflow_bar_color", "overflow_border_color",
    "overflow_background_color"};
const QStringList kAltOrder = {"bar_color", "border_color",
                               "overflow_bar_color", "overflow_border_color"};

QString configPath()
{
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + "/.config";
    return base + "/xwob/xwob.ini";
}

// ---- file model ------------------------------------------------------------

struct Settings
{
    std::map<QString, QString> global = kDefaults;
    std::map<QString, QString> alt = kAltDefaults;
    QStringList extraGlobal;  // unknown global lines, kept verbatim
    QStringList extraAlt;     // unknown [style.alt] lines (e.g. its backgrounds)
    QStringList otherSections; // [output.*], [style.NAME]: Wayland only

    static Settings load(const QString &path)
    {
        Settings s;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return s;
        enum { Global, Alt, Other } where = Global;
        const QString text = QString::fromUtf8(f.readAll());
        for (QString line : text.split('\n'))
        {
            line.remove('\r');
            const QString t = line.trimmed();
            if (t.startsWith('['))
            {
                if (t == "[style.alt]")
                    where = Alt;
                else
                {
                    where = Other;
                    s.otherSections << line;
                }
                continue;
            }
            if (where == Other)
            {
                s.otherSections << line;
                continue;
            }
            if (t.isEmpty() || t.startsWith('#') || t.startsWith(';'))
                continue; // comments of keys we rewrite are not kept
            const int eq = t.indexOf('=');
            if (eq < 0)
                continue;
            const QString key = t.left(eq).trimmed();
            QString val = t.mid(eq + 1).trimmed();
            const int semi = val.indexOf(QRegularExpression("\\s;"));
            if (semi >= 0)
                val = val.left(semi).trimmed();
            if (where == Global)
            {
                if (kDefaults.count(key))
                    s.global[key] = val;
                else
                    s.extraGlobal << t;
            }
            else
            {
                if (kAltDefaults.count(key))
                    s.alt[key] = val;
                else
                    s.extraAlt << t;
            }
        }
        while (!s.otherSections.isEmpty() && s.otherSections.last().trimmed().isEmpty())
            s.otherSections.removeLast();
        return s;
    }

    QString serialize(const std::map<QString, QString> &override = {}) const
    {
        QString out =
            "# xwob configuration, written by xwob-settings.\n"
            "# One file for both backends (X11 and Wayland); see man xwob.\n"
            "# Hand edits are fine: unknown keys and Wayland-only sections are kept.\n\n";
        for (const QString &k : kGlobalOrder)
        {
            auto o = override.find(k);
            out += k + " = " + (o != override.end() ? o->second : global.at(k)) + "\n";
        }
        for (const QString &l : extraGlobal)
            out += l + "\n";
        out += "\n[style.alt]\n";
        for (const QString &k : kAltOrder)
            out += k + " = " + alt.at(k) + "\n";
        for (const QString &l : extraAlt)
            out += l + "\n";
        if (!otherSections.isEmpty())
            out += "\n" + otherSections.join("\n") + "\n";
        return out;
    }
};

// ---- widgets ---------------------------------------------------------------

// RRGGBB or RRGGBBAA as used by the config file
QString colorToHex(const QColor &c)
{
    QString s = QString("%1%2%3")
                    .arg(c.red(), 2, 16, QChar('0'))
                    .arg(c.green(), 2, 16, QChar('0'))
                    .arg(c.blue(), 2, 16, QChar('0'));
    if (c.alpha() != 255)
        s += QString("%1").arg(c.alpha(), 2, 16, QChar('0'));
    return s;
}

bool hexToColor(const QString &h, QColor *out)
{
    static const QRegularExpression re("^[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$");
    if (!re.match(h).hasMatch())
        return false;
    bool ok;
    out->setRgb(h.mid(0, 2).toInt(&ok, 16), h.mid(2, 2).toInt(&ok, 16),
                h.mid(4, 2).toInt(&ok, 16),
                h.size() == 8 ? h.mid(6, 2).toInt(&ok, 16) : 255);
    return true;
}

class ColorButton : public QPushButton
{
public:
    std::function<void()> onChange;

    explicit ColorButton(QWidget *parent = nullptr) : QPushButton(parent)
    {
        setMinimumWidth(110);
        setCursor(Qt::PointingHandCursor);
        QObject::connect(this, &QPushButton::clicked, this, [this] {
            QColor c = QColorDialog::getColor(
                color_, this, "Choose colour", QColorDialog::ShowAlphaChannel);
            if (c.isValid() && c != color_)
            {
                setHex(colorToHex(c));
                if (onChange)
                    onChange();
            }
        });
    }
    QString hex() const { return colorToHex(color_); }
    void setHex(const QString &h)
    {
        QColor c;
        if (!hexToColor(h, &c))
            return;
        color_ = c;
        setText("#" + colorToHex(c));
        // swatch over a checkerboard-ish neutral so transparency is visible
        const int lum = (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000;
        const bool dark = c.alpha() > 100 ? lum < 140 : false;
        setStyleSheet(QString("QPushButton{background-color:rgba(%1,%2,%3,%4);"
                              "border:1px solid #888;padding:4px;color:%5;}")
                          .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha())
                          .arg(dark ? "white" : "black"));
    }

private:
    QColor color_;
};

struct Field
{
    QString key;
    bool alt;
    std::function<QString()> get;
    std::function<void(const QString &)> set;
};

} // namespace

// ---- main window -----------------------------------------------------------

class Window : public QWidget
{
public:
    Window(const QString &path, const QString &xwobBin)
        : path_(path), xwob_(xwobBin)
    {
        setWindowTitle("xwob settings");
        auto *root = new QVBoxLayout(this);
        auto *tabs = new QTabWidget;
        root->addWidget(tabs);

        tabs->addTab(buildLayoutTab(), "Layout");
        tabs->addTab(buildColorsTab(), "Colours");
        tabs->addTab(buildBehaviourTab(), "Behaviour");
        root->addWidget(buildPreviewBar());

        status_ = new QLabel;
        status_->setWordWrap(true);
        root->addWidget(status_);

        auto *buttons = new QHBoxLayout;
        auto *applyBtn = new QPushButton("Apply");
        applyBtn->setDefault(true);
        applyBtn->setToolTip("Save xwob.ini and reload running bars (Ctrl+S)");
        auto *revertBtn = new QPushButton("Revert");
        revertBtn->setToolTip("Reload the settings from the file");
        auto *defaultsBtn = new QPushButton("Defaults");
        defaultsBtn->setToolTip("Put back the built-in defaults (not saved until Apply)");
        auto *closeBtn = new QPushButton("Close");
        buttons->addWidget(defaultsBtn);
        buttons->addWidget(revertBtn);
        buttons->addStretch();
        buttons->addWidget(applyBtn);
        buttons->addWidget(closeBtn);
        root->addLayout(buttons);

        connect_(applyBtn, &QPushButton::clicked, [this] { apply(); });
        connect_(revertBtn, &QPushButton::clicked, [this] { revert(); });
        connect_(defaultsBtn, &QPushButton::clicked, [this] { resetDefaults(); });
        connect_(closeBtn, &QPushButton::clicked, [this] { close_(); });
        auto *sc = new QShortcut(QKeySequence::Save, this);
        QObject::connect(sc, &QShortcut::activated, this, [this] { apply(); });

        debounce_.setSingleShot(true);
        debounce_.setInterval(200);
        QObject::connect(&debounce_, &QTimer::timeout, this, [this] { refreshPreview(); });
        keepAlive_.setInterval(9000); // the preview bar hides after 10 s
        QObject::connect(&keepAlive_, &QTimer::timeout, this, [this] { sendValue(); });

        loading_ = false;
        revert();
    }

    void shutdown() { stopPreview(); }

    // developer helper: render the window to a PNG
    bool grabTo(const QString &file) { return grab().save(file); }

protected:
    void closeEvent(QCloseEvent *e) override
    {
        stopPreview();
        QWidget::closeEvent(e);
    }

private:
    // -- plumbing --
    template <class Obj, class Sig, class Fn>
    void connect_(Obj *o, Sig sig, Fn fn) { QObject::connect(o, sig, this, fn); }
    void close_() { close(); }

    void changed()
    {
        if (loading_)
            return;
        setDirty(true);
        if (live_ && live_->isChecked())
            debounce_.start();
    }

    void setDirty(bool d)
    {
        dirty_ = d;
        setWindowTitle(d ? "xwob settings *" : "xwob settings");
    }

    QSpinBox *spin(const QString &key, int lo, int hi, int step = 1, const QString &suffix = {})
    {
        auto *s = new QSpinBox;
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setSuffix(suffix);
        connect_(s, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) { changed(); });
        fields_.push_back({key, false, [s] { return QString::number(s->value()); },
                           [s](const QString &v) { s->setValue(v.toInt()); }});
        return s;
    }

    QComboBox *combo(const QString &key, const QStringList &items)
    {
        auto *c = new QComboBox;
        c->addItems(items);
        connect_(c, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { changed(); });
        fields_.push_back({key, false, [c] { return c->currentText(); },
                           [c](const QString &v) {
                               const int i = c->findText(v);
                               if (i >= 0)
                                   c->setCurrentIndex(i);
                           }});
        return c;
    }

    ColorButton *color(const QString &key, bool alt)
    {
        auto *b = new ColorButton;
        b->onChange = [this] { changed(); };
        fields_.push_back({key, alt, [b] { return b->hex(); },
                           [b](const QString &v) { b->setHex(v); }});
        return b;
    }

    // -- tabs --
    QWidget *buildLayoutTab()
    {
        auto *w = new QWidget;
        auto *lay = new QVBoxLayout(w);

        auto *size = new QGroupBox("Size (whole bar, outline included)");
        auto *sf = new QFormLayout(size);
        sf->addRow("Orientation", combo("orientation", {"horizontal", "vertical"}));
        sf->addRow("Width", spin("width", 8, 8000, 10, " px"));
        sf->addRow("Height", spin("height", 8, 8000, 10, " px"));
        sf->addRow("Outline gap", spin("border_offset", 0, 200, 1, " px"));
        sf->addRow("Border", spin("border_size", 0, 200, 1, " px"));
        sf->addRow("Padding", spin("bar_padding", 0, 200, 1, " px"));
        lay->addWidget(size);

        auto *pos = new QGroupBox("Position");
        auto *pf = new QFormLayout(pos);
        vAnchor_ = new QComboBox;
        vAnchor_->addItems({"top", "center", "bottom"});
        hAnchor_ = new QComboBox;
        hAnchor_->addItems({"left", "center", "right"});
        connect_(vAnchor_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { changed(); });
        connect_(hAnchor_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { changed(); });
        pf->addRow("Vertical anchor", vAnchor_);
        pf->addRow("Horizontal anchor", hAnchor_);
        const char *names[4] = {"Margin top", "Margin right", "Margin bottom", "Margin left"};
        for (int i = 0; i < 4; i++)
        {
            margin_[i] = new QSpinBox;
            margin_[i]->setRange(0, 8000);
            margin_[i]->setSuffix(" px");
            connect_(margin_[i], QOverload<int>::of(&QSpinBox::valueChanged), [this](int) { changed(); });
            pf->addRow(names[i], margin_[i]);
        }
        lay->addWidget(pos);
        lay->addStretch();

        // anchor and margin are composite keys
        fields_.push_back({"anchor", false,
                           [this] {
                               const QString v = vAnchor_->currentText() == "center" ? "" : vAnchor_->currentText();
                               const QString h = hAnchor_->currentText() == "center" ? "" : hAnchor_->currentText();
                               const QString s = (v + " " + h).trimmed();
                               return s.isEmpty() ? QString("center") : s;
                           },
                           [this](const QString &val) {
                               QStringList t = val.split(' ', Qt::SkipEmptyParts);
                               bool top = t.contains("top"), bot = t.contains("bottom");
                               bool l = t.contains("left"), r = t.contains("right");
                               vAnchor_->setCurrentIndex(top && !bot ? 0 : bot && !top ? 2 : 1);
                               hAnchor_->setCurrentIndex(l && !r ? 0 : r && !l ? 2 : 1);
                           }});
        fields_.push_back({"margin", false,
                           [this] {
                               const int a = margin_[0]->value();
                               bool same = true;
                               for (int i = 1; i < 4; i++)
                                   same = same && margin_[i]->value() == a;
                               if (same)
                                   return QString::number(a);
                               return QString("%1 %2 %3 %4").arg(margin_[0]->value())
                                   .arg(margin_[1]->value()).arg(margin_[2]->value())
                                   .arg(margin_[3]->value());
                           },
                           [this](const QString &val) {
                               QStringList t = val.split(' ', Qt::SkipEmptyParts);
                               int v[4] = {0, 0, 0, 0};
                               if (t.size() == 1)
                                   v[0] = v[1] = v[2] = v[3] = t[0].toInt();
                               else if (t.size() == 4)
                                   for (int i = 0; i < 4; i++)
                                       v[i] = t[i].toInt();
                               for (int i = 0; i < 4; i++)
                                   margin_[i]->setValue(v[i]);
                           }});
        return w;
    }

    QWidget *buildColorsTab()
    {
        auto *w = new QWidget;
        auto *lay = new QVBoxLayout(w);

        auto *normal = new QGroupBox("Normal");
        auto *nf = new QFormLayout(normal);
        nf->addRow("Bar", color("bar_color", false));
        nf->addRow("Border", color("border_color", false));
        nf->addRow("Background", color("background_color", false));
        lay->addWidget(normal);

        auto *over = new QGroupBox("Overflow (value above max)");
        auto *of = new QFormLayout(over);
        of->addRow("Bar", color("overflow_bar_color", false));
        of->addRow("Border", color("overflow_border_color", false));
        of->addRow("Background", color("overflow_background_color", false));
        lay->addWidget(over);

        auto *alt = new QGroupBox("Alt style (input \"50!\" or \"50 alt\")");
        auto *af = new QFormLayout(alt);
        af->addRow("Bar", color("bar_color", true));
        af->addRow("Border", color("border_color", true));
        af->addRow("Overflow bar", color("overflow_bar_color", true));
        af->addRow("Overflow border", color("overflow_border_color", true));
        lay->addWidget(alt);
        lay->addStretch();
        return w;
    }

    QWidget *buildBehaviourTab()
    {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->addRow("Full bar at", spin("max", 1, 10000, 10));
        f->addRow("Hide after", spin("timeout", 1, 10000, 100, " ms"));
        f->addRow("Overflow", combo("overflow_mode", {"wrap", "nowrap"}));
        auto *pct = new QCheckBox("Show \"NN%\" on the bar");
        connect_(pct, &QCheckBox::toggled, [this](bool) { changed(); });
        fields_.push_back({"show_percentage", false,
                           [pct] { return QString(pct->isChecked() ? "yes" : "no"); },
                           [pct](const QString &v) {
                               pct->setChecked(v == "yes" || v == "true" || v == "1");
                           }});
        f->addRow(pct);
        auto *note = new QLabel(
            "wrap: 130 with max 100 shows 30% in the overflow colours.\n"
            "nowrap: the bar stays full, in the overflow colours.\n\n"
            "Settings apply to X11 and Wayland alike.");
        note->setWordWrap(true);
        f->addRow(note);
        return w;
    }

    QWidget *buildPreviewBar()
    {
        auto *box = new QGroupBox("Preview");
        auto *l = new QHBoxLayout(box);
        live_ = new QCheckBox("Live");
        live_->setChecked(true);
        live_->setToolTip("Show the bar with the current, unsaved settings while you edit");
        slider_ = new QSlider(Qt::Horizontal);
        slider_->setRange(0, 150);
        slider_->setValue(50);
        valueLabel_ = new QLabel("50%");
        valueLabel_->setMinimumWidth(40);
        altBox_ = new QCheckBox("Alt");
        auto *show = new QPushButton("Show");
        l->addWidget(live_);
        l->addWidget(slider_, 1);
        l->addWidget(valueLabel_);
        l->addWidget(altBox_);
        l->addWidget(show);

        connect_(slider_, &QSlider::valueChanged, [this](int v) {
            valueLabel_->setText(QString::number(v) + "%");
            if (live_->isChecked() && preview_)
                sendValue();
        });
        connect_(altBox_, &QCheckBox::toggled, [this](bool) {
            if (live_->isChecked())
                sendValue();
        });
        connect_(show, &QPushButton::clicked, [this] { refreshPreview(); });
        connect_(live_, &QCheckBox::toggled, [this](bool on) {
            if (on)
                refreshPreview();
            else
                stopPreview();
        });
        return box;
    }

    // -- model <-> widgets --
    Settings collect() const
    {
        Settings s = model_; // keeps extras
        for (const Field &f : fields_)
            (f.alt ? s.alt : s.global)[f.key] = f.get();
        return s;
    }

    void populate(const Settings &s)
    {
        loading_ = true;
        for (const Field &f : fields_)
            f.set(f.alt ? s.alt.at(f.key) : s.global.at(f.key));
        loading_ = false;
    }

    void revert()
    {
        model_ = Settings::load(path_);
        populate(model_);
        setDirty(false);
        status_->setText(QFileInfo::exists(path_) ? "Loaded " + path_
                                                  : "No config yet: showing the defaults. " + path_ +
                                                        " is created on Apply.");
        if (live_ && live_->isChecked())
            debounce_.start();
    }

    void resetDefaults()
    {
        Settings d;
        d.extraGlobal = model_.extraGlobal;
        d.extraAlt = model_.extraAlt;
        d.otherSections = model_.otherSections;
        populate(d);
        setDirty(true);
        status_->setText("Defaults put back. Apply to save them.");
        if (live_->isChecked())
            debounce_.start();
    }

    // -- actions --
    void apply()
    {
        const Settings s = collect();
        QDir().mkpath(QFileInfo(path_).absolutePath());
        QSaveFile out(path_);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QMessageBox::warning(this, "xwob settings", "Cannot write " + path_ + ":\n" + out.errorString());
            return;
        }
        out.write(s.serialize().toUtf8());
        if (!out.commit())
        {
            QMessageBox::warning(this, "xwob settings", "Cannot write " + path_);
            return;
        }
        model_ = s;
        setDirty(false);

        QString msg = "Saved " + path_ + ". ";
        if (xwob_.isEmpty())
        {
            msg += "xwob was not found, so running bars were not reloaded.";
        }
        else
        {
            QProcess p;
            p.start(xwob_, {"--reload"});
            p.waitForFinished(3000);
            const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
            static const QRegularExpression re("reloaded (\\d+)");
            const auto m = re.match(err);
            if (m.hasMatch() && m.captured(1) != "0")
                msg += "Running bar reloaded: the new look is live.";
            else
                msg += "No running bar to reload; it takes effect the next time xwob starts.";
        }
        status_->setText(msg);
    }

    void startPreviewProcess()
    {
        if (xwob_.isEmpty())
        {
            status_->setText("xwob was not found: set XWOB_BIN or install it next to xwob-settings.");
            return;
        }
        if (!tmp_)
        {
            tmp_ = new QTemporaryFile(QDir::tempPath() + "/xwob-preview-XXXXXX.ini", this);
            if (!tmp_->open())
                return;
        }
        // the bar must stay up while the user edits: longest timeout the format allows
        tmp_->resize(0);
        tmp_->write(collect().serialize({{"timeout", "10000"}}).toUtf8());
        tmp_->flush();

        preview_ = new QProcess(this);
        preview_->setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(preview_, &QProcess::readyRead, this, [this] {
            const QString out = QString::fromUtf8(preview_->readAll());
            for (const QString &l : out.split('\n'))
                if (l.startsWith("Error") || l.startsWith("xwob:") || l.contains("[ERROR]"))
                    status_->setText("Preview: " + l.trimmed());
        });
        preview_->start(xwob_, {"-c", tmp_->fileName()});
    }

    void stopPreview()
    {
        keepAlive_.stop();
        if (!preview_)
            return;
        preview_->disconnect();
        preview_->closeWriteChannel();
        preview_->terminate();
        if (!preview_->waitForFinished(1500))
        {
            preview_->kill();
            preview_->waitForFinished(500);
        }
        delete preview_;
        preview_ = nullptr;
    }

    void refreshPreview()
    {
        stopPreview();
        startPreviewProcess();
        if (!preview_)
            return;
        sendValue();
        keepAlive_.start();
    }

    void sendValue()
    {
        if (!preview_ || preview_->state() == QProcess::NotRunning)
            return;
        const int max = collect().global.at("max").toInt();
        const int v = slider_->value() * max / 100;
        preview_->write((QString::number(v) + (altBox_->isChecked() ? "!" : "") + "\n").toUtf8());
    }

    // -- state --
    QString path_, xwob_;
    Settings model_;
    std::vector<Field> fields_;
    QComboBox *vAnchor_ = nullptr, *hAnchor_ = nullptr;
    QSpinBox *margin_[4] = {};
    QLabel *status_ = nullptr, *valueLabel_ = nullptr;
    QCheckBox *live_ = nullptr, *altBox_ = nullptr;
    QSlider *slider_ = nullptr;
    QTimer debounce_, keepAlive_;
    QProcess *preview_ = nullptr;
    QTemporaryFile *tmp_ = nullptr;
    bool loading_ = true;
    bool dirty_ = false;
};

static QString findXwob()
{
    const QString env = qEnvironmentVariable("XWOB_BIN");
    if (!env.isEmpty())
        return env;
    QString p = QStandardPaths::findExecutable("xwob", {QCoreApplication::applicationDirPath()});
    if (p.isEmpty())
        p = QStandardPaths::findExecutable("xwob");
    return p;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("xwob-settings");
    QString path = configPath();
    QString grab;
    for (int i = 1; i < argc; i++)
    {
        const QString a = argv[i];
        if ((a == "-c" || a == "--config") && i + 1 < argc)
            path = argv[++i];
        else if (a == "--grab" && i + 1 < argc)
            grab = argv[++i];
        else if (a == "-h" || a == "--help")
        {
            printf("Usage: xwob-settings [-c file]\n"
                   "Edit xwob.ini (default: %s).\n"
                   "Environment: XWOB_BIN selects the xwob launcher used for preview and reload.\n",
                   qPrintable(configPath()));
            return 0;
        }
    }

    Window w(path, findXwob());
    w.resize(440, 560);
    w.show();
    if (!grab.isEmpty())
    {
        QTimer::singleShot(300, [&] {
            const bool ok = w.grabTo(grab);
            w.shutdown();
            QApplication::exit(ok ? 0 : 1);
        });
    }
    return app.exec();
}
