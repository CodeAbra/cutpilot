#pragma once

#include "cutpilot/core/Node.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace cutpilot::app {

// The command palette's searchable content: the creatable node taxonomy
// grouped by category, plus the live model registry so a model can be found
// by name and placed as a ready-configured generation node. In offer mode the
// list is restricted to the type-compatible titles a dropped connector
// computed. Plain data with an incremental filter, so it is headless-testable
// apart from any widget.
class PaletteModel {
public:
    enum class RowKind {
        Section,
        Node,
        Model
    };

    struct ModelEntry {
        QString id;
        QString label;
        QString provider;
        bool hasKey = false;
    };

    struct Row {
        RowKind kind = RowKind::Node;
        QString text;
        QString detail;
        core::Node prototype;
        ModelEntry model;
    };

    PaletteModel();

    void setModels(const QVector<ModelEntry> &models);

    // Restrict the list to these node titles (a dropped connector's
    // type-compatible offers); models are excluded in this mode.
    void setOfferTitles(const QStringList &titles);
    void clearOffers();
    bool offersActive() const { return m_offersActive; }

    // The visible rows for a filter string: category sections with their
    // matching nodes, then a models section. Sections never appear empty.
    QVector<Row> rows(const QString &filter) const;

private:
    QVector<ModelEntry> m_models;
    QStringList m_offerTitles;
    bool m_offersActive = false;
};

} // namespace cutpilot::app
