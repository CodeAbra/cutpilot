#include "KeyManagerPanel.h"

#include "cutpilot/ipc/GenerationCoordinator.h"
#include "cutpilot/secrets/SecretStore.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QVBoxLayout>

namespace cutpilot::app {

namespace {

// The keychain item the generation service reads: one generic password per
// vendor under this service name, account = vendor.
QString keychainService()
{
    return QStringLiteral("cutpilot");
}

} // namespace

// One vendor's line: its name, its key state, the add/replace and remove
// controls, and the inline editor and remove confirmation they unfold into.
struct KeyManagerPanel::VendorRow {
    QString provider;
    bool needsKey = false;
    QWidget *root = nullptr;
    QLabel *status = nullptr;
    QPushButton *addReplace = nullptr;
    QPushButton *remove = nullptr;
    QWidget *editorRow = nullptr;
    QLineEdit *editor = nullptr;
    QPushButton *save = nullptr;
    QPushButton *cancel = nullptr;
    QLabel *error = nullptr;
    QWidget *confirmRow = nullptr;
    QPushButton *confirmRemove = nullptr;
    QPushButton *keepKey = nullptr;
};

KeyManagerPanel::KeyManagerPanel(const theme::ThemeTable &theme,
                                 ipc::GenerationCoordinator *coordinator,
                                 secrets::SecretStore *secrets, QWidget *parent)
    : QWidget(parent)
    , m_coordinator(coordinator)
    , m_secrets(secrets)
    , m_theme(&theme)
{
    setAttribute(Qt::WA_StyledBackground, true);

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(10, 8, 10, 8);
    column->setSpacing(6);

    m_empty = new QLabel(
        QStringLiteral("No vendors yet — the model registry loads once the "
                       "generation service is up."),
        this);
    m_empty->setWordWrap(true);
    column->addWidget(m_empty);

    auto *rowsHost = new QWidget(this);
    m_rowsLayout = new QVBoxLayout(rowsHost);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(6);
    column->addWidget(rowsHost);

    connect(m_coordinator, &ipc::GenerationCoordinator::modelsReady, this,
            &KeyManagerPanel::rebuild);

    retheme(theme);
    rebuild();
}

KeyManagerPanel::~KeyManagerPanel()
{
    qDeleteAll(m_rows);
}

void KeyManagerPanel::retheme(const theme::ThemeTable &theme)
{
    m_theme = &theme;
    setStyleSheet(
        QStringLiteral(
            "cutpilot--app--KeyManagerPanel {"
            "  background-color: %6; border: 1px solid %3; border-radius: 8px;"
            "}"
            "QLabel { color: %1; background: transparent; }"
            "QLabel[vendorName=\"true\"] { font-weight: 600; }"
            "QLineEdit {"
            "  background-color: %2; color: %1;"
            "  border: 1px solid %3; border-radius: 4px; padding: 4px 8px;"
            "}"
            "QLineEdit:focus { border: 2px solid %4; }"
            "QPushButton {"
            "  color: %1; background-color: %2;"
            "  border: 1px solid %3; border-radius: 4px; padding: 3px 10px;"
            "}"
            "QPushButton:hover { background-color: %5; }"
            "QPushButton:focus { border: 2px solid %4; }")
            .arg(theme.textPrimary().name(), theme.surface3().name(),
                 theme.borderDefault().name(), theme.borderFocus().name(),
                 theme.surfaceHover().name(), theme.surface1().name()));
    m_empty->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                               .arg(theme.textSecondary().name()));
    for (VendorRow *row : m_rows)
        refreshRow(row);
}

void KeyManagerPanel::openKeyEditor(const QString &provider)
{
    for (VendorRow *row : m_rows) {
        if (row->provider != provider || !row->needsKey)
            continue;
        row->confirmRow->hide();
        row->error->hide();
        row->editorRow->show();
        row->editor->setFocus();
        return;
    }
}

bool KeyManagerPanel::keyLooksPlausible(const QString &key, QString *reason)
{
    const auto fail = [reason](const QString &why) {
        if (reason)
            *reason = why;
        return false;
    };
    const QString trimmed = key.trimmed();
    if (trimmed.isEmpty())
        return fail(QStringLiteral("Paste a key first."));
    if (trimmed.size() < 8)
        return fail(QStringLiteral("That looks too short to be an API key."));
    if (trimmed.size() > 512)
        return fail(QStringLiteral("That looks too long to be an API key."));
    for (const QChar ch : trimmed) {
        const char16_t unit = ch.unicode();
        if (unit <= 0x20 || unit >= 0x7F)
            return fail(QStringLiteral(
                "Keys are one token of plain ASCII, without spaces."));
    }
    if (reason)
        reason->clear();
    return true;
}

bool KeyManagerPanel::anyRowInteracting() const
{
    for (const VendorRow *row : m_rows) {
        if (row->editorRow && !row->editorRow->isHidden())
            return true;
        if (row->confirmRow && !row->confirmRow->isHidden())
            return true;
    }
    return false;
}

void KeyManagerPanel::runDeferredRebuild()
{
    if (!m_rebuildDeferred || anyRowInteracting())
        return;
    m_rebuildDeferred = false;
    // The rebuild deletes the very rows whose controls invoked this, so it
    // runs once the current interaction has unwound rather than in place.
    QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
}

void KeyManagerPanel::rebuild()
{
    // A refresh landing while a row is mid-interaction keeps the open editor
    // and its typed key; the rows' state still updates, and the rebuild runs
    // once the interaction ends.
    if (anyRowInteracting()) {
        m_rebuildDeferred = true;
        for (VendorRow *row : m_rows)
            refreshRow(row);
        return;
    }
    m_rebuildDeferred = false;

    for (VendorRow *row : m_rows)
        delete row->root;
    qDeleteAll(m_rows);
    m_rows.clear();

    // Vendors in registry order of importance: the keyed ones first, each
    // group alphabetical. A vendor is keyed when any of its models is.
    QMap<QString, bool> vendors;
    for (const auto &model : m_coordinator->models())
        vendors[model.provider] = vendors.value(model.provider) || model.needsKey;

    m_empty->setVisible(vendors.isEmpty());

    for (const bool keyedPass : { true, false }) {
        for (auto it = vendors.constBegin(); it != vendors.constEnd(); ++it) {
            if (it.value() != keyedPass)
                continue;
            const QString provider = it.key();

            auto *row = new VendorRow;
            row->provider = provider;
            row->needsKey = it.value();

            row->root = new QWidget(this);
            row->root->setObjectName(QStringLiteral("keyRow-") + provider);
            auto *rowColumn = new QVBoxLayout(row->root);
            rowColumn->setContentsMargins(0, 0, 0, 0);
            rowColumn->setSpacing(4);

            auto *header = new QHBoxLayout;
            header->setSpacing(8);
            auto *name = new QLabel(provider, row->root);
            name->setProperty("vendorName", true);
            row->status = new QLabel(row->root);
            row->status->setObjectName(QStringLiteral("keyStatus-") + provider);
            header->addWidget(name);
            header->addWidget(row->status, 1);

            if (row->needsKey) {
                row->addReplace = new QPushButton(row->root);
                row->addReplace->setObjectName(QStringLiteral("keyAdd-")
                                               + provider);
                row->remove = new QPushButton(QStringLiteral("Remove…"),
                                              row->root);
                row->remove->setObjectName(QStringLiteral("keyRemove-")
                                           + provider);
                header->addWidget(row->addReplace);
                header->addWidget(row->remove);
            }
            rowColumn->addLayout(header);

            if (row->needsKey) {
                row->editorRow = new QWidget(row->root);
                auto *editLine = new QHBoxLayout(row->editorRow);
                editLine->setContentsMargins(0, 0, 0, 0);
                editLine->setSpacing(6);
                row->editor = new QLineEdit(row->editorRow);
                row->editor->setObjectName(QStringLiteral("keyEdit-")
                                           + provider);
                row->editor->setEchoMode(QLineEdit::Password);
                row->editor->setPlaceholderText(
                    QStringLiteral("Paste your %1 API key").arg(provider));
                row->save = new QPushButton(QStringLiteral("Save"),
                                            row->editorRow);
                row->save->setObjectName(QStringLiteral("keySave-") + provider);
                row->cancel = new QPushButton(QStringLiteral("Cancel"),
                                              row->editorRow);
                row->cancel->setObjectName(QStringLiteral("keyCancel-")
                                           + provider);
                editLine->addWidget(row->editor, 1);
                editLine->addWidget(row->save);
                editLine->addWidget(row->cancel);
                row->editorRow->hide();
                rowColumn->addWidget(row->editorRow);

                row->error = new QLabel(row->root);
                row->error->setObjectName(QStringLiteral("keyError-")
                                          + provider);
                row->error->setWordWrap(true);
                row->error->hide();
                rowColumn->addWidget(row->error);

                row->confirmRow = new QWidget(row->root);
                row->confirmRow->setObjectName(QStringLiteral("keyConfirm-")
                                               + provider);
                auto *confirmLine = new QHBoxLayout(row->confirmRow);
                confirmLine->setContentsMargins(0, 0, 0, 0);
                confirmLine->setSpacing(6);
                auto *confirmText = new QLabel(
                    QStringLiteral("This deletes the %1 key from your "
                                   "keychain.")
                        .arg(provider),
                    row->confirmRow);
                confirmText->setWordWrap(true);
                row->confirmRemove = new QPushButton(
                    QStringLiteral("Remove key"), row->confirmRow);
                row->confirmRemove->setObjectName(
                    QStringLiteral("keyConfirmRemove-") + provider);
                row->keepKey = new QPushButton(QStringLiteral("Keep"),
                                               row->confirmRow);
                row->keepKey->setObjectName(QStringLiteral("keyKeepKey-")
                                            + provider);
                confirmLine->addWidget(confirmText, 1);
                confirmLine->addWidget(row->confirmRemove);
                confirmLine->addWidget(row->keepKey);
                row->confirmRow->hide();
                rowColumn->addWidget(row->confirmRow);

                connect(row->addReplace, &QPushButton::clicked, this,
                        [this, row] {
                            row->confirmRow->hide();
                            row->error->hide();
                            row->editorRow->setVisible(
                                !row->editorRow->isVisible());
                            if (row->editorRow->isVisible()) {
                                row->editor->setFocus();
                            } else {
                                row->editor->clear();
                                runDeferredRebuild();
                            }
                        });
                connect(row->editor, &QLineEdit::returnPressed, this,
                        [this, row] { saveKey(row); });
                connect(row->save, &QPushButton::clicked, this,
                        [this, row] { saveKey(row); });
                connect(row->cancel, &QPushButton::clicked, this,
                        [this, row] {
                            row->editor->clear();
                            row->editorRow->hide();
                            row->error->hide();
                            runDeferredRebuild();
                        });
                connect(row->remove, &QPushButton::clicked, this,
                        [this, row] {
                            row->editorRow->hide();
                            row->error->hide();
                            row->confirmRow->show();
                        });
                connect(row->confirmRemove, &QPushButton::clicked, this,
                        [this, row] { removeKey(row); });
                connect(row->keepKey, &QPushButton::clicked, this,
                        [this, row] {
                            row->confirmRow->hide();
                            runDeferredRebuild();
                        });
            }

            m_rowsLayout->addWidget(row->root);
            m_rows.push_back(row);
            refreshRow(row);
        }
    }
}

void KeyManagerPanel::refreshRow(VendorRow *row)
{
    const bool inKeychain =
        row->needsKey && m_secrets->hasSecret(keychainService(), row->provider);
    bool registryHasKey = false;
    for (const auto &model : m_coordinator->models()) {
        if (model.provider == row->provider && model.needsKey && model.hasKey)
            registryHasKey = true;
    }

    QString text;
    QColor color;
    QString hint;
    if (!row->needsKey) {
        text = QStringLiteral("No key required");
        color = m_theme->textTertiary();
    } else if (registryHasKey && inKeychain) {
        text = QStringLiteral("Key set");
        color = m_theme->statusDone();
    } else if (registryHasKey) {
        text = QStringLiteral("Key set — from environment");
        color = m_theme->statusDone();
        hint = QStringLiteral(
            "This key comes from an environment variable, so it can't be "
            "removed here. A keychain key takes over once the variable is "
            "unset.");
    } else if (inKeychain) {
        text = QStringLiteral("Key stored — waiting for the service to see it");
        color = m_theme->statusInfo();
    } else {
        text = QStringLiteral("No key");
        color = m_theme->statusWarning();
    }
    row->status->setText(text);
    row->status->setToolTip(hint);
    row->status->setStyleSheet(
        QStringLiteral("color: %1; background: transparent;")
            .arg(color.name()));

    if (row->needsKey) {
        row->addReplace->setText((inKeychain || registryHasKey)
                                     ? QStringLiteral("Replace key…")
                                     : QStringLiteral("Add key…"));
        row->remove->setVisible(inKeychain);
        row->error->setStyleSheet(
            QStringLiteral("color: %1; background: transparent;")
                .arg(m_theme->statusError().name()));
    }
}

void KeyManagerPanel::saveKey(VendorRow *row)
{
    QString reason;
    const QString key = row->editor->text().trimmed();
    if (!keyLooksPlausible(key, &reason)) {
        row->error->setText(reason);
        row->error->show();
        return;
    }
    if (!m_secrets->available()
        || !m_secrets->writeSecret(keychainService(), row->provider, key)) {
        row->error->setText(
            QStringLiteral("The keychain refused the write. Set the vendor's "
                           "API key environment variable and relaunch."));
        row->error->show();
        return;
    }
    row->editor->clear();
    row->editorRow->hide();
    row->error->hide();
    refreshRow(row);
    m_coordinator->refreshModels();
    emit keyStored(row->provider);
}

void KeyManagerPanel::removeKey(VendorRow *row)
{
    if (!m_secrets->removeSecret(keychainService(), row->provider)) {
        row->confirmRow->hide();
        row->error->setText(QStringLiteral("The keychain refused the delete."));
        row->error->show();
        return;
    }
    row->confirmRow->hide();
    row->error->hide();
    refreshRow(row);
    m_coordinator->refreshModels();
    emit keyRemoved(row->provider);
}

} // namespace cutpilot::app
