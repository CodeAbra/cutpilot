#include "PaletteModel.h"

#include "cutpilot/core/NodeCatalog.h"

namespace cutpilot::app {

namespace {

bool matches(const QString &haystack, const QString &needle)
{
    return haystack.contains(needle, Qt::CaseInsensitive);
}

} // namespace

PaletteModel::PaletteModel() = default;

void PaletteModel::setModels(const QVector<ModelEntry> &models)
{
    m_models = models;
}

void PaletteModel::setOfferTitles(const QStringList &titles)
{
    m_offerTitles = titles;
    m_offersActive = true;
}

void PaletteModel::clearOffers()
{
    m_offerTitles.clear();
    m_offersActive = false;
}

QVector<PaletteModel::Row> PaletteModel::rows(const QString &filter) const
{
    const QString needle = filter.trimmed();
    QVector<Row> rows;

    if (m_offersActive) {
        // Offer mode lists exactly the compatible titles, still searchable.
        for (const QString &title : m_offerTitles) {
            if (!needle.isEmpty() && !matches(title, needle))
                continue;
            Row row;
            row.kind = RowKind::Node;
            row.text = title;
            row.prototype = core::catalogPrototype(title);
            rows.push_back(row);
        }
        return rows;
    }

    QString openSection;
    for (const core::CatalogEntry &entry : core::nodeCatalog()) {
        const bool hit = needle.isEmpty()
            || matches(entry.prototype.title, needle)
            || matches(entry.category, needle)
            || matches(entry.prototype.modelLabel, needle);
        if (!hit)
            continue;
        if (entry.category != openSection) {
            openSection = entry.category;
            Row section;
            section.kind = RowKind::Section;
            section.text = entry.category;
            rows.push_back(section);
        }
        Row row;
        row.kind = RowKind::Node;
        row.text = entry.prototype.title;
        row.detail = entry.prototype.modelLabel;
        row.prototype = entry.prototype;
        rows.push_back(row);
    }

    bool modelsOpen = false;
    for (const ModelEntry &model : m_models) {
        const bool hit = needle.isEmpty() || matches(model.label, needle)
            || matches(model.provider, needle) || matches(model.id, needle);
        if (!hit)
            continue;
        if (!modelsOpen) {
            modelsOpen = true;
            Row section;
            section.kind = RowKind::Section;
            section.text = QStringLiteral("Models");
            rows.push_back(section);
        }
        Row row;
        row.kind = RowKind::Model;
        row.text = model.label;
        row.detail = model.hasKey || !model.provider.isEmpty()
            ? (model.hasKey ? model.provider
                            : model.provider + QStringLiteral(" — add key"))
            : QString();
        row.model = model;
        // Picking a model places a generation node already set to it.
        row.prototype = core::catalogPrototype(QStringLiteral("Generate Image"));
        row.prototype.modelId = model.id;
        row.prototype.modelLabel = model.label;
        rows.push_back(row);
    }

    return rows;
}

} // namespace cutpilot::app
