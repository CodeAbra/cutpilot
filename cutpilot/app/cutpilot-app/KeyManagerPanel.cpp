#include "KeyManagerPanel.h"

#include "cutpilot/ipc/GenerationCoordinator.h"
#include "cutpilot/secrets/SecretStore.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVector>

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
// controls, and the inline editor and remove confirmation they unfold into. A
// vendor may need more than one secret, so the editor holds one masked field
// per slot; a single-secret vendor keeps the shipped one-field layout.
struct KeyManagerPanel::VendorRow {
    // One masked field of the editor, bound to the keychain account the service
    // supplied. The account is never constructed on this side.
    struct SlotWidget {
        QString name;
        QString account;
        QLabel *label = nullptr;
        QLineEdit *editor = nullptr;
    };

    QString provider;
    bool needsKey = false;
    QVector<SlotWidget> slotFields;
    QWidget *root = nullptr;
    QLabel *status = nullptr;
    QPushButton *addReplace = nullptr;
    QPushButton *remove = nullptr;
    QWidget *editorRow = nullptr;
    // The first slot's field, for focus and visibility checks; every slot's
    // field lives in `slotFields`.
    QLineEdit *editor = nullptr;
    QPushButton *save = nullptr;
    QPushButton *cancel = nullptr;
    QLabel *error = nullptr;
    QWidget *confirmRow = nullptr;
    QPushButton *confirmRemove = nullptr;
    QPushButton *keepKey = nullptr;

    int slotsPresent(secrets::SecretStore *store, const QString &service) const
    {
        int present = 0;
        for (const SlotWidget &slot : slotFields) {
            if (store->hasSecret(service, slot.account))
                ++present;
        }
        return present;
    }
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
    connect(m_coordinator, &ipc::GenerationCoordinator::keyVendorsReady, this,
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
        m_pendingEditorProvider.clear();
        return;
    }
    // No row for this vendor yet — the registry may still be loading. Open the
    // editor once the next rebuild creates the row.
    m_pendingEditorProvider = provider;
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
    // group alphabetical. A vendor is keyed when any of its models is. The key
    // surface sources the key-vendor channel, so an unconfirmed vendor is
    // key-registrable here without ever entering the picker. If a service omits
    // that channel, fall back to the model registry so the shipped
    // single-secret vendors still render rather than leaving the surface empty;
    // an unconfirmed vendor simply stays absent, exactly as before the channel
    // existed.
    const QVector<ipc::ModelInfo> &keyVendors = m_coordinator->keyVendors();
    const QVector<ipc::ModelInfo> &vendorSource =
        keyVendors.isEmpty() ? m_coordinator->models() : keyVendors;
    QMap<QString, bool> vendors;
    QHash<QString, QVector<ipc::SecretSlot>> slotsByProvider;
    for (const auto &model : vendorSource) {
        vendors[model.provider] = vendors.value(model.provider) || model.needsKey;
        if (!slotsByProvider.contains(model.provider) && !model.secretSlots.isEmpty())
            slotsByProvider.insert(model.provider, model.secretSlots);
    }

    m_empty->setVisible(vendors.isEmpty());

    for (const bool keyedPass : { true, false }) {
        for (auto it = vendors.constBegin(); it != vendors.constEnd(); ++it) {
            if (it.value() != keyedPass)
                continue;
            const QString provider = it.key();

            auto *row = new VendorRow;
            row->provider = provider;
            row->needsKey = it.value();
            const QVector<ipc::SecretSlot> providerSlots =
                slotsByProvider.value(provider);

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

                const bool multi = providerSlots.size() > 1;
                if (!multi) {
                    // A single-secret vendor keeps the shipped one-field layout
                    // and object names, so every existing control resolves. Its
                    // account is the slot's when the channel supplied one, else
                    // the provider id (the shipped scheme).
                    auto *editLine = new QHBoxLayout(row->editorRow);
                    editLine->setContentsMargins(0, 0, 0, 0);
                    editLine->setSpacing(6);
                    auto *field = new QLineEdit(row->editorRow);
                    field->setObjectName(QStringLiteral("keyEdit-") + provider);
                    field->setEchoMode(QLineEdit::Password);
                    field->setPlaceholderText(
                        QStringLiteral("Paste your %1 API key").arg(provider));
                    row->save = new QPushButton(QStringLiteral("Save"),
                                                row->editorRow);
                    row->save->setObjectName(QStringLiteral("keySave-")
                                             + provider);
                    row->cancel = new QPushButton(QStringLiteral("Cancel"),
                                                  row->editorRow);
                    row->cancel->setObjectName(QStringLiteral("keyCancel-")
                                               + provider);
                    editLine->addWidget(field, 1);
                    editLine->addWidget(row->save);
                    editLine->addWidget(row->cancel);
                    const QString account = providerSlots.isEmpty()
                                                ? provider
                                                : providerSlots.first().account;
                    row->slotFields.push_back(
                        { providerSlots.isEmpty()
                              ? QStringLiteral("key")
                              : providerSlots.first().name,
                          account, nullptr, field });
                    row->editor = field;
                } else {
                    // A multi-secret vendor stacks one labeled masked field per
                    // slot, sharing one Save / Cancel. Each field's account is
                    // the one the service supplied, written back verbatim.
                    auto *editColumn = new QVBoxLayout(row->editorRow);
                    editColumn->setContentsMargins(0, 0, 0, 0);
                    editColumn->setSpacing(6);
                    for (const ipc::SecretSlot &slot : providerSlots) {
                        auto *slotLine = new QHBoxLayout;
                        slotLine->setSpacing(6);
                        auto *slotLabel = new QLabel(slot.label, row->editorRow);
                        auto *field = new QLineEdit(row->editorRow);
                        field->setObjectName(QStringLiteral("keyEdit-")
                                             + provider + QStringLiteral("-")
                                             + slot.name);
                        field->setEchoMode(QLineEdit::Password);
                        field->setPlaceholderText(slot.label);
                        slotLine->addWidget(slotLabel);
                        slotLine->addWidget(field, 1);
                        editColumn->addLayout(slotLine);
                        row->slotFields.push_back(
                            { slot.name, slot.account, slotLabel, field });
                    }
                    auto *buttonLine = new QHBoxLayout;
                    buttonLine->setSpacing(6);
                    row->save = new QPushButton(QStringLiteral("Save"),
                                                row->editorRow);
                    row->save->setObjectName(QStringLiteral("keySave-")
                                             + provider);
                    row->cancel = new QPushButton(QStringLiteral("Cancel"),
                                                  row->editorRow);
                    row->cancel->setObjectName(QStringLiteral("keyCancel-")
                                               + provider);
                    buttonLine->addStretch(1);
                    buttonLine->addWidget(row->save);
                    buttonLine->addWidget(row->cancel);
                    editColumn->addLayout(buttonLine);
                    row->editor = row->slotFields.first().editor;
                }
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

                const auto clearFields = [row] {
                    for (const VendorRow::SlotWidget &slot : row->slotFields)
                        slot.editor->clear();
                };
                connect(row->addReplace, &QPushButton::clicked, this,
                        [this, row, clearFields] {
                            row->confirmRow->hide();
                            row->error->hide();
                            row->editorRow->setVisible(
                                !row->editorRow->isVisible());
                            if (row->editorRow->isVisible()) {
                                row->editor->setFocus();
                            } else {
                                clearFields();
                                runDeferredRebuild();
                            }
                        });
                for (const VendorRow::SlotWidget &slot : row->slotFields) {
                    connect(slot.editor, &QLineEdit::returnPressed, this,
                            [this, row] { saveKey(row); });
                }
                connect(row->save, &QPushButton::clicked, this,
                        [this, row] { saveKey(row); });
                connect(row->cancel, &QPushButton::clicked, this,
                        [this, row, clearFields] {
                            clearFields();
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

    if (!m_pendingEditorProvider.isEmpty()) {
        const QString provider = m_pendingEditorProvider;
        m_pendingEditorProvider.clear();
        openKeyEditor(provider);
    }
}

void KeyManagerPanel::refreshRow(VendorRow *row)
{
    const int total = row->slotFields.size();
    const int present =
        row->needsKey ? row->slotsPresent(m_secrets, keychainService()) : 0;
    // Every slot present is a full key; some but not all is a partial set. The
    // Replace / Remove controls key off any stored slot.
    const bool inKeychain = row->needsKey && total > 0 && present == total;
    const bool anyStored = present > 0;
    const QVector<ipc::ModelInfo> &keyVendors = m_coordinator->keyVendors();
    const QVector<ipc::ModelInfo> &presenceSource =
        keyVendors.isEmpty() ? m_coordinator->models() : keyVendors;
    bool registryHasKey = false;
    for (const auto &model : presenceSource) {
        if (model.provider == row->provider && model.needsKey && model.hasKey)
            registryHasKey = true;
    }

    // A save that landed only part of the key locally waits here: once the
    // service reports the whole key present (its remaining slots supplied by
    // the environment), the run-unblock fires exactly once.
    if (registryHasKey && m_pendingUnblock.remove(row->provider))
        emit keyStored(row->provider);

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
    } else if (anyStored) {
        text = QStringLiteral("Partially set — %1 of %2").arg(present).arg(total);
        color = m_theme->statusWarning();
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
        row->addReplace->setText(anyStored
                                     ? QStringLiteral("Replace key…")
                                     : QStringLiteral("Add key…"));
        row->remove->setVisible(anyStored);
        row->error->setStyleSheet(
            QStringLiteral("color: %1; background: transparent;")
                .arg(m_theme->statusError().name()));
    }
}

void KeyManagerPanel::saveKey(VendorRow *row)
{
    // Collect every filled field; an empty field is left to a later save, so a
    // multi-secret vendor can land one credential now and the rest later.
    struct Filled {
        QString account;
        QString value;
    };
    QVector<Filled> filled;
    QString reason;
    for (const VendorRow::SlotWidget &slot : row->slotFields) {
        const QString value = slot.editor->text().trimmed();
        if (value.isEmpty())
            continue;
        if (!keyLooksPlausible(value, &reason)) {
            const QString label =
                slot.label ? slot.label->text() : QString();
            row->error->setText(label.isEmpty()
                                    ? reason
                                    : label + QStringLiteral(": ") + reason);
            row->error->show();
            return;
        }
        filled.push_back({ slot.account, value });
    }
    if (filled.isEmpty()) {
        // Nothing to write; surface the same guidance an empty single field
        // gives so the user knows to paste a key.
        keyLooksPlausible(QString(), &reason);
        row->error->setText(reason);
        row->error->show();
        return;
    }

    if (!m_secrets->available()) {
        row->error->setText(
            QStringLiteral("The keychain refused the write. Set the vendor's "
                           "API key environment variable and relaunch."));
        row->error->show();
        return;
    }
    for (const Filled &item : filled) {
        if (!m_secrets->writeSecret(keychainService(), item.account,
                                    item.value)) {
            row->error->setText(
                QStringLiteral("The keychain refused the write. Set the "
                               "vendor's API key environment variable and "
                               "relaunch."));
            row->error->show();
            return;
        }
    }

    for (const VendorRow::SlotWidget &slot : row->slotFields)
        slot.editor->clear();
    row->editorRow->hide();
    row->error->hide();
    refreshRow(row);

    // Only a complete key unblocks a run: the service refuses a job whose
    // provider is missing any secret. Every slot present in the keychain is a
    // full key on its own, so the run-unblock fires at once and holds even when
    // the service is down. Otherwise the remaining slots may come from the
    // environment: re-pull the registry and let the refreshed vendor channel's
    // presence signal decide, so a mixed env+keychain key still unblocks.
    m_coordinator->refreshModels();
    const bool keychainComplete =
        !row->slotFields.isEmpty()
        && row->slotsPresent(m_secrets, keychainService())
               == row->slotFields.size();
    if (keychainComplete) {
        m_pendingUnblock.remove(row->provider);
        runDeferredRebuild();
        emit keyStored(row->provider);
    } else {
        m_pendingUnblock.insert(row->provider);
        runDeferredRebuild();
    }
}

void KeyManagerPanel::removeKey(VendorRow *row)
{
    // Delete every stored slot for the vendor; a partly-stored multi-secret
    // vendor is cleared whole. Absent slots need no delete.
    for (const VendorRow::SlotWidget &slot : row->slotFields) {
        if (!m_secrets->hasSecret(keychainService(), slot.account))
            continue;
        if (!m_secrets->removeSecret(keychainService(), slot.account)) {
            row->confirmRow->hide();
            row->error->setText(
                QStringLiteral("The keychain refused the delete."));
            row->error->show();
            return;
        }
    }
    m_pendingUnblock.remove(row->provider);
    row->confirmRow->hide();
    row->error->hide();
    refreshRow(row);
    m_coordinator->refreshModels();
    runDeferredRebuild();
    emit keyRemoved(row->provider);
}

} // namespace cutpilot::app
