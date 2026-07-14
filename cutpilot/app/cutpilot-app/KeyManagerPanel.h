#pragma once

#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace cutpilot::ipc {
class GenerationCoordinator;
}
namespace cutpilot::secrets {
class SecretStore;
}
namespace cutpilot::theme {
class ThemeTable;
}

namespace cutpilot::app {

// The BYOK key surface: every vendor the model registry knows, each with
// its key state and add/replace/remove controls. A key lands in the same
// keychain entry the generation service reads (service "cutpilot",
// account = vendor), so a key added here is immediately the run path's
// key. The surface only ever shows whether a key exists — never the key
// itself, in any state, label, or tooltip.
class KeyManagerPanel : public QWidget {
    Q_OBJECT

public:
    KeyManagerPanel(const theme::ThemeTable &theme,
                    ipc::GenerationCoordinator *coordinator,
                    secrets::SecretStore *secrets, QWidget *parent = nullptr);
    ~KeyManagerPanel() override;

    void retheme(const theme::ThemeTable &theme);

    // Open one vendor's inline key editor, e.g. arriving from a blocked
    // node's add-a-key affordance. Unknown vendors are ignored.
    void openKeyEditor(const QString &provider);

    // Format-level plausibility only — no vendor is contacted and nothing
    // is claimed about the key being live. A false result explains itself
    // in `reason`.
    static bool keyLooksPlausible(const QString &key, QString *reason = nullptr);

signals:
    // A key for the vendor was written to the keychain. The model registry
    // refresh is already on its way when this fires.
    void keyStored(const QString &provider);
    void keyRemoved(const QString &provider);

private:
    struct VendorRow;

    void rebuild();
    void refreshRow(VendorRow *row);
    void saveKey(VendorRow *row);
    void removeKey(VendorRow *row);

    // A row is mid-interaction while its editor or remove-confirmation is
    // showing; a registry refresh must not tear such a row down under the
    // user.
    bool anyRowInteracting() const;
    // Run a rebuild that was held back while a row was mid-interaction, once
    // the interaction has ended.
    void runDeferredRebuild();

    ipc::GenerationCoordinator *m_coordinator = nullptr;
    secrets::SecretStore *m_secrets = nullptr;
    const theme::ThemeTable *m_theme = nullptr;
    QVBoxLayout *m_rowsLayout = nullptr;
    QLabel *m_empty = nullptr;
    QVector<VendorRow *> m_rows;
    bool m_rebuildDeferred = false;
};

} // namespace cutpilot::app
