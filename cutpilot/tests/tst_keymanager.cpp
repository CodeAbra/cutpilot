#include <QtTest/QtTest>

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPair>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "KeyManagerPanel.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/ipc/GenerationClient.h"
#include "cutpilot/ipc/GenerationCoordinator.h"
#include "cutpilot/ipc/SidecarHost.h"
#include "cutpilot/secrets/SecretStore.h"
#include "cutpilot/theme/ThemeTable.h"

using namespace cutpilot;
using cutpilot::app::KeyManagerPanel;

namespace {

// The keychain scheme the surface and the generation service share.
const QString kService = QStringLiteral("cutpilot");
const QString kOpenai = QStringLiteral("openai");
const QString kKling = QStringLiteral("kling");
const QString kKlingAccess = QStringLiteral("kling.access_key");
const QString kKlingSecret = QStringLiteral("kling.secret_key");

// Obviously non-real placeholder secrets. They exist only inside the process:
// the in-memory store below and, for the environment cases, a presence-only
// boolean the service reports back. No real vendor key is ever used.
const QString kPlausibleKey = QStringLiteral("plausible-key-abcdefgh01234567");
const QString kReplacementKey = QStringLiteral("replacement-key-zyxwvut98765432");
const QString kKlingAccessKey = QStringLiteral("kling-access-abcdefgh01234567");
const QString kKlingSecretKey = QStringLiteral("kling-secret-zyxwvut98765432");

// Clears OPENAI_API_KEY when the scope exits, even if an assertion aborts the
// slot early, so an environment key set for one case never leaks into a later
// one.
struct OpenAiEnvGuard {
    ~OpenAiEnvGuard() { qunsetenv("OPENAI_API_KEY"); }
};

} // namespace

// A SecretStore that lives entirely in memory. Injected in place of the
// keychain-backed store so no test ever reads, writes, or deletes a real
// keychain item.
class FakeSecretStore final : public secrets::SecretStore {
public:
    bool available() const override { return true; }

    bool writeSecret(const QString &service, const QString &account,
                     const QString &value) override
    {
        m_items.insert({ service, account }, value);
        return true;
    }

    QString readSecret(const QString &service,
                       const QString &account) const override
    {
        return m_items.value({ service, account });
    }

    bool removeSecret(const QString &service, const QString &account) override
    {
        m_items.remove({ service, account });
        return true;
    }

    bool hasSecret(const QString &service,
                   const QString &account) const override
    {
        return m_items.contains({ service, account });
    }

    int itemCount() const { return m_items.size(); }

    void seed(const QString &service, const QString &account,
              const QString &value)
    {
        m_items.insert({ service, account }, value);
    }

private:
    QMap<QPair<QString, QString>, QString> m_items;
};

class KeyManagerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void keyLooksPlausibleAcceptsAndRejectsByFormat();
    void addWritesToTheStoreAndClearsTheField();
    void replaceOverwritesTheSingleItem();
    void removeDeletesAfterConfirmation();
    void entryFieldIsMasked();
    void collapsingTheEditorClearsTheTypedKey();
    void savingAKeyFiresTheStoredSignal();
    void noSecretLeaksIntoTheSurface();
    void keyStatusReflectsEnvironmentAndKeychain();
    void editSurvivesARegistryRefresh();
    void saveFlushesADeferredRebuildWhenTheServiceIsDown();
    void multiSecretRowRendersALabeledMaskedFieldPerSlot();
    void savingEveryMultiSecretFieldWritesEachAccountAndFiresStoredOnce();
    void aPartialMultiSecretSaveStoresOneAndDoesNotUnblock();
    void removingAMultiSecretVendorDeletesEverySlot();
    void noMultiSecretValueLeaksIntoTheSurface();
    void anUnconfirmedVendorIsKeyRegistrableButNotPickable();
    void captureSurfaceImage();

private:
    struct Rig {
        theme::ThemeTable table{ theme::Theme::Dark };
        core::NodeGraph graph;
        ipc::GenerationCoordinator coordinator;
        FakeSecretStore secrets;
        KeyManagerPanel panel;

        explicit Rig(ipc::GenerationClient *client)
            : coordinator(&graph, client)
            , panel(table, &coordinator, &secrets, nullptr)
        {
        }

        QLineEdit *editor() const
        {
            return panel.findChild<QLineEdit *>(QStringLiteral("keyEdit-")
                                                + kOpenai);
        }
        QPushButton *save() const
        {
            return panel.findChild<QPushButton *>(QStringLiteral("keySave-")
                                                  + kOpenai);
        }
        QPushButton *addReplace() const
        {
            return panel.findChild<QPushButton *>(QStringLiteral("keyAdd-")
                                                  + kOpenai);
        }
        QPushButton *remove() const
        {
            return panel.findChild<QPushButton *>(QStringLiteral("keyRemove-")
                                                  + kOpenai);
        }
        QPushButton *confirmRemove() const
        {
            return panel.findChild<QPushButton *>(
                QStringLiteral("keyConfirmRemove-") + kOpenai);
        }
        QLabel *status() const
        {
            return panel.findChild<QLabel *>(QStringLiteral("keyStatus-")
                                             + kOpenai);
        }

        QLineEdit *klingAccess() const
        {
            return panel.findChild<QLineEdit *>(
                QStringLiteral("keyEdit-kling-access_key"));
        }
        QLineEdit *klingSecret() const
        {
            return panel.findChild<QLineEdit *>(
                QStringLiteral("keyEdit-kling-secret_key"));
        }
        QPushButton *klingSave() const
        {
            return panel.findChild<QPushButton *>(QStringLiteral("keySave-")
                                                  + kKling);
        }
        QPushButton *klingRemove() const
        {
            return panel.findChild<QPushButton *>(QStringLiteral("keyRemove-")
                                                  + kKling);
        }
        QPushButton *klingConfirmRemove() const
        {
            return panel.findChild<QPushButton *>(
                QStringLiteral("keyConfirmRemove-") + kKling);
        }
        QLabel *klingStatus() const
        {
            return panel.findChild<QLabel *>(QStringLiteral("keyStatus-")
                                             + kKling);
        }
    };

    bool waitForModels(Rig &rig)
    {
        QSignalSpy modelsSpy(&rig.coordinator,
                             &ipc::GenerationCoordinator::modelsReady);
        rig.coordinator.serviceBecameReady();
        return QTest::qWaitFor([&] { return modelsSpy.count() >= 1; }, 10000);
    }

    QTemporaryDir m_genDir;
    ipc::SidecarHost m_host;
    ipc::GenerationClient m_client;
};

void KeyManagerTest::initTestCase()
{
    // Keychain lookups are disabled so the service keys presence off the
    // environment alone, and the vendor key is unset so the keyless states
    // are guaranteed keyless.
    qputenv("CUTPILOT_DISABLE_KEYCHAIN", "1");
    qunsetenv("OPENAI_API_KEY");
    QVERIFY(m_genDir.isValid());
    qputenv("CUTPILOT_GEN_DIR", m_genDir.path().toUtf8());

    QSignalSpy readySpy(&m_host, &ipc::SidecarHost::ready);
    QSignalSpy failedSpy(&m_host, &ipc::SidecarHost::failed);
    m_host.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() == 1 || failedSpy.count() == 1,
                             15000);
    if (failedSpy.count() > 0)
        QFAIL(qPrintable(failedSpy.first().first().toString()));
    m_client.setEndpoint(m_host.port(), m_host.token());
}

void KeyManagerTest::cleanupTestCase()
{
    m_host.stop();
}

void KeyManagerTest::keyLooksPlausibleAcceptsAndRejectsByFormat()
{
    QString reason;
    QVERIFY(!KeyManagerPanel::keyLooksPlausible(QString(), &reason));
    QVERIFY(!reason.isEmpty());
    QVERIFY(!KeyManagerPanel::keyLooksPlausible(QStringLiteral("short12")));
    QVERIFY(!KeyManagerPanel::keyLooksPlausible(QString(513, QLatin1Char('a'))));
    QVERIFY(!KeyManagerPanel::keyLooksPlausible(
        QStringLiteral("has a space in it")));
    QVERIFY(!KeyManagerPanel::keyLooksPlausible(
        QStringLiteral("with\nnewline00")));
    QVERIFY(!KeyManagerPanel::keyLooksPlausible(
        QString::fromUtf8("caf\xC3\xA9-not-ascii-000")));

    reason = QStringLiteral("stale");
    QVERIFY(KeyManagerPanel::keyLooksPlausible(QStringLiteral("abcd1234"),
                                               &reason));
    QVERIFY(reason.isEmpty());
    QVERIFY(KeyManagerPanel::keyLooksPlausible(kPlausibleKey));
}

void KeyManagerTest::addWritesToTheStoreAndClearsTheField()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    rig.panel.openKeyEditor(kOpenai);
    QLineEdit *editor = rig.editor();
    QVERIFY(editor);
    editor->setText(kPlausibleKey);
    rig.save()->click();

    QVERIFY(rig.secrets.hasSecret(kService, kOpenai));
    QVERIFY(editor->text().isEmpty());
    QLabel *status = rig.status();
    QVERIFY(status);
    QVERIFY(status->text().contains(QStringLiteral("waiting")));
}

void KeyManagerTest::replaceOverwritesTheSingleItem()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    rig.panel.openKeyEditor(kOpenai);
    rig.editor()->setText(kPlausibleKey);
    rig.save()->click();

    rig.panel.openKeyEditor(kOpenai);
    rig.editor()->setText(kReplacementKey);
    rig.save()->click();

    // One item, overwritten in place — not a second entry. Values are checked
    // with QVERIFY so a failure never prints the secret.
    QCOMPARE(rig.secrets.itemCount(), 1);
    QVERIFY(rig.secrets.readSecret(kService, kOpenai) == kReplacementKey);
    QVERIFY(rig.secrets.readSecret(kService, kOpenai) != kPlausibleKey);
}

void KeyManagerTest::removeDeletesAfterConfirmation()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    rig.panel.openKeyEditor(kOpenai);
    rig.editor()->setText(kPlausibleKey);
    rig.save()->click();
    QVERIFY(rig.secrets.hasSecret(kService, kOpenai));

    rig.remove()->click();
    rig.confirmRemove()->click();

    QVERIFY(!rig.secrets.hasSecret(kService, kOpenai));
    QCOMPARE(rig.status()->text(), QStringLiteral("No key"));
}

void KeyManagerTest::entryFieldIsMasked()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    rig.panel.openKeyEditor(kOpenai);
    QLineEdit *editor = rig.editor();
    QVERIFY(editor);
    QCOMPARE(editor->echoMode(), QLineEdit::Password);
}

void KeyManagerTest::collapsingTheEditorClearsTheTypedKey()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    // The Add/Replace toggle keys off live visibility, so the surface must be
    // shown for the collapse branch to run as it does in the app's dialog.
    rig.panel.show();

    // Open the inline editor via Add/Replace, type a key, then dismiss it by
    // toggling Add/Replace off again without saving.
    QPushButton *addReplace = rig.addReplace();
    QVERIFY(addReplace);
    addReplace->click();
    QLineEdit *editor = rig.editor();
    QVERIFY(editor);
    QVERIFY(editor->isVisibleTo(&rig.panel));
    editor->setText(kPlausibleKey);
    addReplace->click();

    // The editor is dismissed and holds no secret, bounding the key's
    // in-memory lifetime to the moment the user dismissed it. The field is
    // checked with QVERIFY so a failure never prints the key.
    QLineEdit *after = rig.editor();
    QVERIFY(after);
    QVERIFY(!after->isVisibleTo(&rig.panel));
    QVERIFY(after->text().isEmpty());
}

void KeyManagerTest::savingAKeyFiresTheStoredSignal()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    // The stored signal is the seam a blocked node's rerun hangs off: proving
    // it fires with the right vendor proves that wiring can be made without a
    // live key.
    QSignalSpy storedSpy(&rig.panel, &KeyManagerPanel::keyStored);
    QString rerunProvider;
    connect(&rig.panel, &KeyManagerPanel::keyStored, this,
            [&](const QString &provider) { rerunProvider = provider; });

    rig.panel.openKeyEditor(kOpenai);
    rig.editor()->setText(kPlausibleKey);
    rig.save()->click();

    QCOMPARE(storedSpy.count(), 1);
    QCOMPARE(storedSpy.first().first().toString(), kOpenai);
    QCOMPARE(rerunProvider, kOpenai);
}

void KeyManagerTest::noSecretLeaksIntoTheSurface()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    rig.panel.openKeyEditor(kOpenai);
    rig.editor()->setText(kPlausibleKey);
    rig.save()->click();

    // No rendered text or tooltip anywhere in the panel carries the raw key,
    // and the editor is empty again. The key is never passed to QVERIFY's
    // message, so a failure cannot print it either.
    for (const QLabel *label : rig.panel.findChildren<QLabel *>()) {
        QVERIFY(!label->text().contains(kPlausibleKey));
        QVERIFY(!label->toolTip().contains(kPlausibleKey));
    }
    for (const QLineEdit *field : rig.panel.findChildren<QLineEdit *>()) {
        QVERIFY(!field->text().contains(kPlausibleKey));
        QVERIFY(!field->placeholderText().contains(kPlausibleKey));
    }
}

void KeyManagerTest::keyStatusReflectsEnvironmentAndKeychain()
{
    // A second service process that inherits the vendor key from the
    // environment, so it reports the vendor as keyed. The value is a
    // presence-only placeholder; the service echoes back only a boolean.
    qputenv("OPENAI_API_KEY", "env-presence-placeholder-000000");
    OpenAiEnvGuard envGuard;
    ipc::SidecarHost keyedHost;
    QSignalSpy readySpy(&keyedHost, &ipc::SidecarHost::ready);
    QSignalSpy failedSpy(&keyedHost, &ipc::SidecarHost::failed);
    keyedHost.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() == 1 || failedSpy.count() == 1,
                             15000);
    if (failedSpy.count() > 0)
        QFAIL(qPrintable(failedSpy.first().first().toString()));
    ipc::GenerationClient keyedClient;
    keyedClient.setEndpoint(keyedHost.port(), keyedHost.token());

    // Key from the environment only: the row says so and offers no removal —
    // an environment key cannot be deleted from the surface.
    {
        Rig rig(&keyedClient);
        QVERIFY(waitForModels(rig));
        QLabel *status = rig.status();
        QVERIFY(status);
        QVERIFY(status->text().contains(QStringLiteral("from environment")));
        QVERIFY(rig.remove());
        QVERIFY(!rig.remove()->isVisibleTo(&rig.panel));
    }

    // Environment key and a stored keychain key together read as fully set,
    // and removal is offered for the keychain item.
    {
        Rig rig(&keyedClient);
        rig.secrets.seed(kService, kOpenai,
                         QStringLiteral("keychain-placeholder-1111"));
        QVERIFY(waitForModels(rig));
        QCOMPARE(rig.status()->text(), QStringLiteral("Key set"));
        QVERIFY(rig.remove() && rig.remove()->isVisibleTo(&rig.panel));
    }

    keyedHost.stop();
}

void KeyManagerTest::editSurvivesARegistryRefresh()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    rig.panel.openKeyEditor(kOpenai);
    QLineEdit *editor = rig.editor();
    QVERIFY(editor);
    const QString partial = QStringLiteral("half-typed-key-in-progress-99");
    editor->setText(partial);

    // A registry refresh arrives while the key is still being typed.
    QSignalSpy modelsSpy(&rig.coordinator,
                         &ipc::GenerationCoordinator::modelsReady);
    rig.coordinator.refreshModels();
    QVERIFY(QTest::qWaitFor([&] { return modelsSpy.count() >= 1; }, 10000));

    // The editor is still open and the typed key was not lost.
    QLineEdit *after = rig.editor();
    QVERIFY(after);
    QVERIFY(after->isVisibleTo(&rig.panel));
    QVERIFY(after->text() == partial);
}

void KeyManagerTest::saveFlushesADeferredRebuildWhenTheServiceIsDown()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    rig.panel.openKeyEditor(kOpenai);
    QLineEdit *editor = rig.editor();
    QVERIFY(editor);
    editor->setText(kPlausibleKey);

    // A registry refresh lands while the editor is open, so a full rebuild is
    // deferred until the interaction ends rather than tearing the row down.
    QSignalSpy modelsSpy(&rig.coordinator,
                         &ipc::GenerationCoordinator::modelsReady);
    rig.coordinator.refreshModels();
    QVERIFY(QTest::qWaitFor([&] { return modelsSpy.count() >= 1; }, 10000));

    // The deferred rebuild recreates every row, so the open row's editor is
    // what must be torn down once that rebuild finally runs.
    QPointer<QLineEdit> deferredEditor = rig.editor();
    QVERIFY(deferredEditor);

    // The service drops before the save, so the post-save refresh is a no-op
    // and no fresh registry can round-trip back. The deferred rebuild must
    // still be flushed by the save itself.
    rig.coordinator.serviceBecameUnavailable(QStringLiteral("offline"));
    rig.save()->click();

    QVERIFY(rig.secrets.hasSecret(kService, kOpenai));
    QVERIFY(QTest::qWaitFor([&] { return deferredEditor.isNull(); }, 5000));
}

void KeyManagerTest::multiSecretRowRendersALabeledMaskedFieldPerSlot()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    QLineEdit *access = rig.klingAccess();
    QLineEdit *secret = rig.klingSecret();
    QVERIFY(access);
    QVERIFY(secret);
    QCOMPARE(access->echoMode(), QLineEdit::Password);
    QCOMPARE(secret->echoMode(), QLineEdit::Password);
    QCOMPARE(access->placeholderText(), QStringLiteral("Access Key"));
    QCOMPARE(secret->placeholderText(), QStringLiteral("Secret Key"));

    QStringList labels;
    for (const QLabel *label : rig.panel.findChildren<QLabel *>())
        labels << label->text();
    QVERIFY(labels.contains(QStringLiteral("Access Key")));
    QVERIFY(labels.contains(QStringLiteral("Secret Key")));
}

void KeyManagerTest::savingEveryMultiSecretFieldWritesEachAccountAndFiresStoredOnce()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    QSignalSpy storedSpy(&rig.panel, &KeyManagerPanel::keyStored);
    rig.panel.openKeyEditor(kKling);
    rig.klingAccess()->setText(kKlingAccessKey);
    rig.klingSecret()->setText(kKlingSecretKey);
    rig.klingSave()->click();

    QVERIFY(rig.secrets.hasSecret(kService, kKlingAccess));
    QVERIFY(rig.secrets.hasSecret(kService, kKlingSecret));
    // The accounts are written verbatim from the channel. Values are compared
    // with QVERIFY so a failure never prints a secret.
    QVERIFY(rig.secrets.readSecret(kService, kKlingAccess) == kKlingAccessKey);
    QVERIFY(rig.secrets.readSecret(kService, kKlingSecret) == kKlingSecretKey);
    QCOMPARE(storedSpy.count(), 1);
    QCOMPARE(storedSpy.first().first().toString(), kKling);
}

void KeyManagerTest::aPartialMultiSecretSaveStoresOneAndDoesNotUnblock()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    QSignalSpy storedSpy(&rig.panel, &KeyManagerPanel::keyStored);
    rig.panel.openKeyEditor(kKling);
    rig.klingAccess()->setText(kKlingAccessKey);
    // The secret field is left empty.
    rig.klingSave()->click();

    QVERIFY(rig.secrets.hasSecret(kService, kKlingAccess));
    QVERIFY(!rig.secrets.hasSecret(kService, kKlingSecret));
    // A partial key must not fire the run-unblock signal — the service refuses
    // a job whose provider is missing a secret.
    QCOMPARE(storedSpy.count(), 0);
    QCOMPARE(rig.klingStatus()->text(),
             QStringLiteral("Partially set — 1 of 2"));
}

void KeyManagerTest::removingAMultiSecretVendorDeletesEverySlot()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    rig.panel.openKeyEditor(kKling);
    rig.klingAccess()->setText(kKlingAccessKey);
    rig.klingSecret()->setText(kKlingSecretKey);
    rig.klingSave()->click();
    QVERIFY(rig.secrets.hasSecret(kService, kKlingAccess));
    QVERIFY(rig.secrets.hasSecret(kService, kKlingSecret));

    rig.klingRemove()->click();
    rig.klingConfirmRemove()->click();

    QVERIFY(!rig.secrets.hasSecret(kService, kKlingAccess));
    QVERIFY(!rig.secrets.hasSecret(kService, kKlingSecret));
}

void KeyManagerTest::noMultiSecretValueLeaksIntoTheSurface()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    rig.panel.openKeyEditor(kKling);
    rig.klingAccess()->setText(kKlingAccessKey);
    rig.klingSecret()->setText(kKlingSecretKey);
    rig.klingSave()->click();

    // Neither secret survives in any rendered text, tooltip, placeholder, or
    // object name across both fields. Secrets are never passed to QVERIFY's
    // message, so a failure cannot print them either.
    for (const QLabel *label : rig.panel.findChildren<QLabel *>()) {
        QVERIFY(!label->text().contains(kKlingAccessKey));
        QVERIFY(!label->text().contains(kKlingSecretKey));
        QVERIFY(!label->toolTip().contains(kKlingAccessKey));
        QVERIFY(!label->toolTip().contains(kKlingSecretKey));
    }
    for (const QLineEdit *field : rig.panel.findChildren<QLineEdit *>()) {
        QVERIFY(!field->text().contains(kKlingAccessKey));
        QVERIFY(!field->text().contains(kKlingSecretKey));
        QVERIFY(!field->placeholderText().contains(kKlingAccessKey));
        QVERIFY(!field->placeholderText().contains(kKlingSecretKey));
    }
}

void KeyManagerTest::anUnconfirmedVendorIsKeyRegistrableButNotPickable()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    // Kling is a key-registrable vendor: it reaches the key surface's source.
    bool klingInVendors = false;
    for (const auto &vendor : rig.coordinator.keyVendors()) {
        if (vendor.provider == kKling)
            klingInVendors = true;
    }
    QVERIFY(klingInVendors);

    // It is absent from the picker / runnable source, so it can never be
    // selected or run while unconfirmed.
    for (const auto &model : rig.coordinator.models())
        QVERIFY(model.provider != kKling);

    // The panel renders a key row for it; a picker built from models() has none.
    QVERIFY(rig.panel.findChild<QWidget *>(QStringLiteral("keyRow-") + kKling));
}

void KeyManagerTest::captureSurfaceImage()
{
    // Opt-in rendering of the populated surface to an image, for a visual
    // record without a pointer. The key is a presence-only placeholder and is
    // never shown by the surface, so the image carries no secret.
    const QByteArray outDir = qgetenv("CUTPILOT_SHOT_DIR");
    if (outDir.isEmpty())
        QSKIP("set CUTPILOT_SHOT_DIR to render the surface image");

    qputenv("OPENAI_API_KEY", "env-presence-placeholder-000000");
    OpenAiEnvGuard envGuard;
    ipc::SidecarHost keyedHost;
    QSignalSpy readySpy(&keyedHost, &ipc::SidecarHost::ready);
    QSignalSpy failedSpy(&keyedHost, &ipc::SidecarHost::failed);
    keyedHost.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() == 1 || failedSpy.count() == 1,
                             15000);
    if (failedSpy.count() > 0)
        QFAIL(qPrintable(failedSpy.first().first().toString()));
    ipc::GenerationClient keyedClient;
    keyedClient.setEndpoint(keyedHost.port(), keyedHost.token());

    Rig rig(&keyedClient);
    rig.secrets.seed(kService, kOpenai,
                     QStringLiteral("keychain-placeholder-1111"));
    QVERIFY(waitForModels(rig));

    rig.panel.resize(460, rig.panel.sizeHint().height());
    rig.panel.ensurePolished();
    QTest::qWait(50);
    const QPixmap shot = rig.panel.grab();
    const QString path =
        QString::fromUtf8(outDir) + QStringLiteral("/byok-key-surface.png");
    QVERIFY(shot.save(path));

    keyedHost.stop();
}

QTEST_MAIN(KeyManagerTest)
#include "tst_keymanager.moc"
